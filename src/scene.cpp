#include "scene.h"

#include <cstdio>

// ---------------------------------------------------------------------------
// Handle packing: EntityId = [30:12] generation | [11:0] slot. 0 is invalid;
// a live id always has generation >= 1.
// ---------------------------------------------------------------------------
namespace {

constexpr uint32_t kSlotBits = 12;
constexpr uint32_t kSlotMask = (1u << kSlotBits) - 1;
constexpr int kGenerationWrap = 1 << 19;
constexpr uint32_t kNoComponent = 0xFFFFFFFFu;

EntityId MakeEntityId(int slot, int generation) {
    return (EntityId)(((uint32_t)generation << kSlotBits) | ((uint32_t)slot & kSlotMask));
}
int EntityIdSlot(EntityId id) { return (int)((uint32_t)id & kSlotMask); }
int EntityIdGeneration(EntityId id) { return (int)((uint32_t)id >> kSlotBits); }

void CopyName(char *dst, const char *src) {
    int i = 0;
    if (src != nullptr) {
        for (; src[i] != 0 && i < 63; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = 0;
}

} // namespace

// ---------------------------------------------------------------------------
// SceneCommand payloads. One PayloadStorage rides in each undo-ring slot, so no
// separate allocator: overwriting a command reuses its payload slot.
// ---------------------------------------------------------------------------
enum {
    SceneCmdTag_Create = 1,
    SceneCmdTag_Delete,
    SceneCmdTag_SetTransform,
    SceneCmdTag_TransformSelection,
};

struct PayloadCreate {
    EntityKind kind;
    char name[64];
    EntityId result;
};

struct PayloadDelete {
    EntityId id;
    Entity entity;
    MeshRenderer mesh;
    AudioSource audio;
    AudioListener listener;
    bool wasActiveListener;
};

struct PayloadSetTransform {
    EntityId id;
    Transform before;
    Transform after;
};

struct PayloadTransformSelection {
    uint32_t editStart;
    uint32_t editCount;
};

union PayloadStorage {
    PayloadCreate create;
    PayloadDelete del;
    PayloadSetTransform setTransform;
    PayloadTransformSelection transformSelection;
};

struct TransformEdit {
    EntityId entity;
    Transform before;
    Transform after;
};

// ---------------------------------------------------------------------------
struct SceneState {
    Arena *arena;
    int toolMode; // ToolMode; stored as int so SceneToolModePtr can alias it

    // Entity pool: sparse, indexed by slot. Free slots live on a stack.
    Entity entities[kMaxEntities];
    int entityGeneration[kMaxEntities];
    bool entityInUse[kMaxEntities];
    int entityFreeStack[kMaxEntities];
    int entityFreeTop;

    // Component pools: dense, swap-removed. Entity.component is the dense index.
    MeshRenderer meshRenderers[kMaxMeshRenderers];
    int meshRendererCount;
    AudioSource audioSources[kMaxAudioSources];
    int audioSourceCount;
    AudioListener audioListeners[kMaxAudioListeners];
    int audioListenerCount;

    EntityId activeListener;

    // Selection (shared entity + clip set; never on the undo stack).
    SelectionItem selection[kMaxSelection];
    int selectionCount;
    EntityId activeEntity;

    // Unified undo ring.
    SceneCommand commands[kMaxUndoCommands];
    PayloadStorage payloads[kMaxUndoCommands];
    int commandBase;   // ring index of the oldest stored command
    int commandCount;  // stored commands from base (done + available-for-redo)
    int commandCursor; // how many of those are currently applied

    // Side ring feeding multi-select transform undo.
    TransformEdit transformEdits[kMaxTransformEdits];
    uint32_t transformEditHead;

    // Gizmo drag snapshot.
    bool dragActive;
    int dragCount;
    EntityId dragTargets[kMaxEntities];
    Transform dragBase[kMaxEntities];
};

// ---------------------------------------------------------------------------
// Entity + component pool internals
// ---------------------------------------------------------------------------
namespace {

Entity *EntityMut(SceneState *scene, EntityId id) {
    if (!EntityIdValid(id)) {
        return nullptr;
    }
    int slot = EntityIdSlot(id);
    if (slot < 0 || slot >= kMaxEntities || !scene->entityInUse[slot]) {
        return nullptr;
    }
    if (scene->entities[slot].id != id) {
        return nullptr;
    }
    return &scene->entities[slot];
}

const Entity *EntityConst(const SceneState *scene, EntityId id) {
    return EntityMut(const_cast<SceneState *>(scene), id);
}

uint32_t ComponentAlloc(SceneState *scene, EntityKind kind, EntityId owner) {
    switch (kind) {
    case EntityKind_Mesh: {
        if (scene->meshRendererCount >= kMaxMeshRenderers) {
            fprintf(stderr, "scene: mesh-renderer pool full, entity has no mesh\n");
            return kNoComponent;
        }
        uint32_t dense = (uint32_t)scene->meshRendererCount++;
        MeshRenderer &mesh = scene->meshRenderers[dense];
        mesh = {};
        mesh.entity = owner;
        mesh.mesh = MeshId_Cube;
        mesh.localAabbMin = Vec3{-0.5f, -0.5f, -0.5f};
        mesh.localAabbMax = Vec3{0.5f, 0.5f, 0.5f};
        return dense;
    }
    case EntityKind_AudioSource: {
        if (scene->audioSourceCount >= kMaxAudioSources) {
            fprintf(stderr, "scene: audio-source pool full\n");
            return kNoComponent;
        }
        uint32_t dense = (uint32_t)scene->audioSourceCount++;
        AudioSource &source = scene->audioSources[dense];
        source = {};
        source.entity = owner;
        source.params = AudioSourceParamsDefault();
        return dense;
    }
    case EntityKind_AudioListener: {
        if (scene->audioListenerCount >= kMaxAudioListeners) {
            fprintf(stderr, "scene: audio-listener pool full\n");
            return kNoComponent;
        }
        uint32_t dense = (uint32_t)scene->audioListenerCount++;
        AudioListener &listener = scene->audioListeners[dense];
        listener = {};
        listener.entity = owner;
        return dense;
    }
    }
    return kNoComponent;
}

void ComponentFree(SceneState *scene, EntityKind kind, uint32_t dense) {
    if (dense == kNoComponent) {
        return;
    }
    switch (kind) {
    case EntityKind_Mesh: {
        uint32_t last = (uint32_t)--scene->meshRendererCount;
        if (dense != last) {
            scene->meshRenderers[dense] = scene->meshRenderers[last];
            Entity *moved = EntityMut(scene, scene->meshRenderers[dense].entity);
            if (moved != nullptr) {
                moved->component = dense;
            }
        }
        return;
    }
    case EntityKind_AudioSource: {
        uint32_t last = (uint32_t)--scene->audioSourceCount;
        if (dense != last) {
            scene->audioSources[dense] = scene->audioSources[last];
            Entity *moved = EntityMut(scene, scene->audioSources[dense].entity);
            if (moved != nullptr) {
                moved->component = dense;
            }
        }
        return;
    }
    case EntityKind_AudioListener: {
        uint32_t last = (uint32_t)--scene->audioListenerCount;
        if (dense != last) {
            scene->audioListeners[dense] = scene->audioListeners[last];
            Entity *moved = EntityMut(scene, scene->audioListeners[dense].entity);
            if (moved != nullptr) {
                moved->component = dense;
            }
        }
        return;
    }
    }
}

void SelectionDropEntity(SceneState *scene, EntityId id); // fwd

EntityId EntityAlloc(SceneState *scene, EntityKind kind, const char *name, Transform transform) {
    if (scene->entityFreeTop == 0) {
        fprintf(stderr, "scene: entity pool full (%d), create dropped\n", kMaxEntities);
        return kInvalidEntityId;
    }
    int slot = scene->entityFreeStack[--scene->entityFreeTop];
    int generation = scene->entityGeneration[slot];
    if (generation <= 0) {
        generation = 1;
    }
    EntityId id = MakeEntityId(slot, generation);

    Entity &entity = scene->entities[slot];
    entity = {};
    entity.id = id;
    CopyName(entity.name, name);
    entity.transform = transform;
    entity.kind = kind;
    entity.component = kNoComponent;
    scene->entityInUse[slot] = true;

    entity.component = ComponentAlloc(scene, kind, id);
    return id;
}

void EntityDestroy(SceneState *scene, EntityId id) {
    Entity *entity = EntityMut(scene, id);
    if (entity == nullptr) {
        return;
    }
    ComponentFree(scene, entity->kind, entity->component);

    int slot = EntityIdSlot(id);
    scene->entityInUse[slot] = false;
    scene->entities[slot].id = kInvalidEntityId;
    int generation = scene->entityGeneration[slot] + 1;
    if (generation >= kGenerationWrap) {
        generation = 1;
    }
    scene->entityGeneration[slot] = generation;
    scene->entityFreeStack[scene->entityFreeTop++] = slot;

    SelectionDropEntity(scene, id);
    if (scene->activeListener == id) {
        scene->activeListener = kInvalidEntityId;
    }
}

// Re-inserts a deleted entity at its original slot + generation so undo of a
// delete round-trips the exact EntityId.
void EntityRestore(SceneState *scene, const PayloadDelete *saved) {
    int slot = EntityIdSlot(saved->id);
    if (slot < 0 || slot >= kMaxEntities) {
        return;
    }
    for (int i = 0; i < scene->entityFreeTop; ++i) {
        if (scene->entityFreeStack[i] == slot) {
            scene->entityFreeStack[i] = scene->entityFreeStack[--scene->entityFreeTop];
            break;
        }
    }
    scene->entityInUse[slot] = true;
    scene->entities[slot] = saved->entity;
    scene->entities[slot].id = saved->id;
    scene->entityGeneration[slot] = EntityIdGeneration(saved->id);

    EntityKind kind = saved->entity.kind;
    uint32_t dense = kNoComponent;
    if (kind == EntityKind_Mesh && scene->meshRendererCount < kMaxMeshRenderers) {
        dense = (uint32_t)scene->meshRendererCount++;
        scene->meshRenderers[dense] = saved->mesh;
        scene->meshRenderers[dense].entity = saved->id;
    } else if (kind == EntityKind_AudioSource && scene->audioSourceCount < kMaxAudioSources) {
        dense = (uint32_t)scene->audioSourceCount++;
        scene->audioSources[dense] = saved->audio;
        scene->audioSources[dense].entity = saved->id;
    } else if (kind == EntityKind_AudioListener && scene->audioListenerCount < kMaxAudioListeners) {
        dense = (uint32_t)scene->audioListenerCount++;
        scene->audioListeners[dense] = saved->listener;
        scene->audioListeners[dense].entity = saved->id;
    }
    scene->entities[slot].component = dense;

    if (saved->wasActiveListener) {
        scene->activeListener = saved->id;
    }
}

Vec3 EntityWorldPos(const SceneState *scene, EntityId id) {
    const Entity *entity = EntityConst(scene, id);
    return entity != nullptr ? entity->transform.position : Vec3{0.0f, 0.0f, 0.0f};
}

} // namespace

// ---------------------------------------------------------------------------
// World AABB + ray/frustum math
// ---------------------------------------------------------------------------
namespace {

struct Aabb {
    Vec3 min;
    Vec3 max;
};

Aabb EntityWorldAabb(const SceneState *scene, const Entity *entity) {
    Vec3 localMin = Vec3{-kNonMeshPickHalfExtent, -kNonMeshPickHalfExtent, -kNonMeshPickHalfExtent};
    Vec3 localMax = Vec3{kNonMeshPickHalfExtent, kNonMeshPickHalfExtent, kNonMeshPickHalfExtent};
    if (entity->kind == EntityKind_Mesh && entity->component != kNoComponent) {
        const MeshRenderer &mesh = scene->meshRenderers[entity->component];
        localMin = mesh.localAabbMin;
        localMax = mesh.localAabbMax;
    }

    Mat4 model = TransformToMat4(entity->transform);
    Aabb world;
    bool first = true;
    for (int corner = 0; corner < 8; ++corner) {
        Vec3 local = Vec3{(corner & 1) ? localMax.x : localMin.x, (corner & 2) ? localMax.y : localMin.y,
                          (corner & 4) ? localMax.z : localMin.z};
        Vec3 p = Vec3{
            model.m[0] * local.x + model.m[4] * local.y + model.m[8] * local.z + model.m[12],
            model.m[1] * local.x + model.m[5] * local.y + model.m[9] * local.z + model.m[13],
            model.m[2] * local.x + model.m[6] * local.y + model.m[10] * local.z + model.m[14],
        };
        if (first) {
            world.min = p;
            world.max = p;
            first = false;
        } else {
            world.min = Vec3{fminf(world.min.x, p.x), fminf(world.min.y, p.y), fminf(world.min.z, p.z)};
            world.max = Vec3{fmaxf(world.max.x, p.x), fmaxf(world.max.y, p.y), fmaxf(world.max.z, p.z)};
        }
    }
    return world;
}

bool RayAabb(Ray ray, const Aabb &box, float *outDistance) {
    float tMin = 0.0f;
    float tMax = 1e30f;
    const float *origin = &ray.origin.x;
    const float *dir = &ray.dir.x;
    const float *boxMin = &box.min.x;
    const float *boxMax = &box.max.x;
    for (int axis = 0; axis < 3; ++axis) {
        if (fabsf(dir[axis]) < 1e-8f) {
            if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis]) {
                return false;
            }
            continue;
        }
        float inv = 1.0f / dir[axis];
        float t1 = (boxMin[axis] - origin[axis]) * inv;
        float t2 = (boxMax[axis] - origin[axis]) * inv;
        if (t1 > t2) {
            float swap = t1;
            t1 = t2;
            t2 = swap;
        }
        tMin = fmaxf(tMin, t1);
        tMax = fminf(tMax, t2);
        if (tMin > tMax) {
            return false;
        }
    }
    *outDistance = tMin;
    return true;
}

// Six inward-pointing planes (xyz = normal, w = offset) of the sub-frustum
// `viewProj` carves from the NDC rect.
struct Frustum {
    float planes[6][4];
};

Frustum FrustumFromRect(Mat4 viewProj, NdcRect rect) {
    float scaleX = 2.0f / (rect.maxX - rect.minX);
    float scaleY = 2.0f / (rect.maxY - rect.minY);
    Mat4 crop = Mat4Identity();
    crop.m[0] = scaleX;
    crop.m[5] = scaleY;
    crop.m[12] = -(rect.maxX + rect.minX) / (rect.maxX - rect.minX);
    crop.m[13] = -(rect.maxY + rect.minY) / (rect.maxY - rect.minY);

    Mat4 m = Mat4Multiply(crop, viewProj);
    float row[4][4];
    for (int r = 0; r < 4; ++r) {
        row[r][0] = m.m[0 + r];
        row[r][1] = m.m[4 + r];
        row[r][2] = m.m[8 + r];
        row[r][3] = m.m[12 + r];
    }
    int signs[6] = {1, -1, 1, -1, 1, -1};
    int rows[6] = {0, 0, 1, 1, 2, 2};
    Frustum frustum;
    for (int p = 0; p < 6; ++p) {
        int rr = rows[p];
        float s = (float)signs[p];
        float a = row[3][0] + s * row[rr][0];
        float b = row[3][1] + s * row[rr][1];
        float c = row[3][2] + s * row[rr][2];
        float d = row[3][3] + s * row[rr][3];
        float len = sqrtf(a * a + b * b + c * c);
        if (len < 1e-8f) {
            len = 1.0f;
        }
        frustum.planes[p][0] = a / len;
        frustum.planes[p][1] = b / len;
        frustum.planes[p][2] = c / len;
        frustum.planes[p][3] = d / len;
    }
    return frustum;
}

bool FrustumIntersectsAabb(const Frustum &frustum, const Aabb &box) {
    for (int p = 0; p < 6; ++p) {
        const float *plane = frustum.planes[p];
        Vec3 positive = Vec3{plane[0] >= 0.0f ? box.max.x : box.min.x,
                             plane[1] >= 0.0f ? box.max.y : box.min.y,
                             plane[2] >= 0.0f ? box.max.z : box.min.z};
        float distance = plane[0] * positive.x + plane[1] * positive.y + plane[2] * positive.z + plane[3];
        if (distance < 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
namespace {

int SelectionFind(const SceneState *scene, SelectionItem item) {
    for (int i = 0; i < scene->selectionCount; ++i) {
        if (SelectionItemEqual(scene->selection[i], item)) {
            return i;
        }
    }
    return -1;
}

void SelectionRecomputeActive(SceneState *scene) {
    if (EntityMut(scene, scene->activeEntity) != nullptr) {
        return;
    }
    scene->activeEntity = kInvalidEntityId;
    for (int i = 0; i < scene->selectionCount; ++i) {
        if (scene->selection[i].kind == SelectionKind_Entity) {
            scene->activeEntity = (EntityId)scene->selection[i].id;
            return;
        }
    }
}

void SelectionInsert(SceneState *scene, SelectionItem item) {
    if (SelectionFind(scene, item) >= 0) {
        return;
    }
    if (scene->selectionCount >= kMaxSelection) {
        fprintf(stderr, "scene: selection full (%d), item dropped\n", kMaxSelection);
        return;
    }
    scene->selection[scene->selectionCount++] = item;
    if (item.kind == SelectionKind_Entity && !EntityIdValid(scene->activeEntity)) {
        scene->activeEntity = (EntityId)item.id;
    }
}

void SelectionErase(SceneState *scene, SelectionItem item) {
    int index = SelectionFind(scene, item);
    if (index < 0) {
        return;
    }
    scene->selection[index] = scene->selection[--scene->selectionCount];
    if (item.kind == SelectionKind_Entity && scene->activeEntity == (EntityId)item.id) {
        scene->activeEntity = kInvalidEntityId;
        SelectionRecomputeActive(scene);
    }
}

void SelectionDropEntity(SceneState *scene, EntityId id) {
    SelectionItem item = {SelectionKind_Entity, (uint32_t)id};
    SelectionErase(scene, item);
}

} // namespace

Selection SceneSelection(const SceneState *scene) {
    return Selection{scene->selection, scene->selectionCount, scene->activeEntity};
}

void SceneSelectionClear(SceneState *scene) {
    scene->selectionCount = 0;
    scene->activeEntity = kInvalidEntityId;
}

void SceneSelectionSet(SceneState *scene, const SelectionItem *items, int count) {
    scene->selectionCount = 0;
    scene->activeEntity = kInvalidEntityId;
    for (int i = 0; i < count; ++i) {
        SelectionInsert(scene, items[i]);
    }
}

void SceneSelectionAdd(SceneState *scene, SelectionItem item) { SelectionInsert(scene, item); }
void SceneSelectionRemove(SceneState *scene, SelectionItem item) { SelectionErase(scene, item); }

void SceneSelectionToggle(SceneState *scene, SelectionItem item) {
    if (SelectionFind(scene, item) >= 0) {
        SelectionErase(scene, item);
    } else {
        SelectionInsert(scene, item);
    }
}

bool SceneSelectionContains(const SceneState *scene, SelectionItem item) {
    return SelectionFind(scene, item) >= 0;
}

Vec3 SceneSelectionCentroid(const SceneState *scene) {
    Vec3 sum = Vec3{0.0f, 0.0f, 0.0f};
    int n = 0;
    for (int i = 0; i < scene->selectionCount; ++i) {
        if (scene->selection[i].kind != SelectionKind_Entity) {
            continue;
        }
        const Entity *entity = EntityConst(scene, (EntityId)scene->selection[i].id);
        if (entity == nullptr) {
            continue;
        }
        sum = Vec3Add(sum, entity->transform.position);
        ++n;
    }
    return n > 0 ? Vec3Scale(sum, 1.0f / (float)n) : Vec3{0.0f, 0.0f, 0.0f};
}

int SceneBoxSelect(SceneState *scene, Mat4 viewProj, NdcRect rect, bool additive) {
    if (rect.maxX < rect.minX) {
        float t = rect.minX;
        rect.minX = rect.maxX;
        rect.maxX = t;
    }
    if (rect.maxY < rect.minY) {
        float t = rect.minY;
        rect.minY = rect.maxY;
        rect.maxY = t;
    }
    Frustum frustum = FrustumFromRect(viewProj, rect);

    if (!additive) {
        SceneSelectionClear(scene);
    }
    int matched = 0;
    for (int slot = 0; slot < kMaxEntities; ++slot) {
        if (!scene->entityInUse[slot]) {
            continue;
        }
        const Entity *entity = &scene->entities[slot];
        Aabb box = EntityWorldAabb(scene, entity);
        if (!FrustumIntersectsAabb(frustum, box)) {
            continue;
        }
        SelectionItem item = {SelectionKind_Entity, (uint32_t)entity->id};
        SelectionInsert(scene, item);
        ++matched;
    }
    return matched;
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------
bool ScenePickRayExcluding(const SceneState *scene, Ray ray, EntityId ignoreEntity, PickResult *out) {
    bool hitAny = false;
    float nearest = 1e30f;
    EntityId nearestEntity = kInvalidEntityId;
    for (int slot = 0; slot < kMaxEntities; ++slot) {
        if (!scene->entityInUse[slot]) {
            continue;
        }
        const Entity *entity = &scene->entities[slot];
        if (entity->id == ignoreEntity) {
            continue;
        }
        Aabb box = EntityWorldAabb(scene, entity);
        float distance = 0.0f;
        if (!RayAabb(ray, box, &distance)) {
            continue;
        }
        if (distance < nearest) {
            nearest = distance;
            nearestEntity = entity->id;
            hitAny = true;
        }
    }
    if (!hitAny) {
        return false;
    }
    out->distance = nearest;
    out->entity = nearestEntity;
    out->hitPoint = Vec3Add(ray.origin, Vec3Scale(ray.dir, nearest));
    return true;
}

bool ScenePickRay(const SceneState *scene, Ray ray, PickResult *out) {
    return ScenePickRayExcluding(scene, ray, kInvalidEntityId, out);
}

// ---------------------------------------------------------------------------
// SceneCommand stack
// ---------------------------------------------------------------------------
namespace {

int PushCommand(SceneState *scene, SceneCommand command) {
    scene->commandCount = scene->commandCursor; // discard any redo branch
    if (scene->commandCount == kMaxUndoCommands) {
        scene->commandBase = (scene->commandBase + 1) % kMaxUndoCommands;
        --scene->commandCount;
    }
    int slot = (scene->commandBase + scene->commandCount) % kMaxUndoCommands;
    scene->commands[slot] = command;
    ++scene->commandCount;
    scene->commandCursor = scene->commandCount;
    return slot;
}

int SubmitTyped(SceneState *scene, int tag, void (*redo)(SceneState *, void *),
                void (*undo)(SceneState *, void *), const PayloadStorage &payload) {
    SceneCommand command = {};
    command.tag = tag;
    command.redo = redo;
    command.undo = undo;
    int slot = PushCommand(scene, command);
    scene->payloads[slot] = payload;
    scene->commands[slot].payload = &scene->payloads[slot];
    scene->commands[slot].redo(scene, scene->commands[slot].payload);
    return slot;
}

// --- create ---
void RedoCreate(SceneState *scene, void *raw) {
    PayloadCreate *p = (PayloadCreate *)raw;
    p->result = EntityAlloc(scene, p->kind, p->name, TransformIdentity());
}
void UndoCreate(SceneState *scene, void *raw) {
    PayloadCreate *p = (PayloadCreate *)raw;
    EntityDestroy(scene, p->result);
}

// --- delete ---
void SnapshotForDelete(SceneState *scene, EntityId id, PayloadDelete *p) {
    *p = {};
    p->id = id;
    const Entity *entity = EntityConst(scene, id);
    if (entity == nullptr) {
        return;
    }
    p->entity = *entity;
    if (entity->component != kNoComponent) {
        if (entity->kind == EntityKind_Mesh) {
            p->mesh = scene->meshRenderers[entity->component];
        } else if (entity->kind == EntityKind_AudioSource) {
            p->audio = scene->audioSources[entity->component];
        } else if (entity->kind == EntityKind_AudioListener) {
            p->listener = scene->audioListeners[entity->component];
        }
    }
    p->wasActiveListener = (scene->activeListener == id);
}
void RedoDelete(SceneState *scene, void *raw) {
    PayloadDelete *p = (PayloadDelete *)raw;
    EntityDestroy(scene, p->id);
}
void UndoDelete(SceneState *scene, void *raw) {
    PayloadDelete *p = (PayloadDelete *)raw;
    EntityRestore(scene, p);
}

// --- set transform (single) ---
void RedoSetTransform(SceneState *scene, void *raw) {
    PayloadSetTransform *p = (PayloadSetTransform *)raw;
    Entity *entity = EntityMut(scene, p->id);
    if (entity != nullptr) {
        entity->transform = p->after;
    }
}
void UndoSetTransform(SceneState *scene, void *raw) {
    PayloadSetTransform *p = (PayloadSetTransform *)raw;
    Entity *entity = EntityMut(scene, p->id);
    if (entity != nullptr) {
        entity->transform = p->before;
    }
}

// --- transform selection (multi) ---
void ApplyTransformEditRange(SceneState *scene, uint32_t start, uint32_t count, bool redo) {
    for (uint32_t i = 0; i < count; ++i) {
        const TransformEdit &edit = scene->transformEdits[(start + i) % kMaxTransformEdits];
        Entity *entity = EntityMut(scene, edit.entity);
        if (entity != nullptr) {
            entity->transform = redo ? edit.after : edit.before;
        }
    }
}
void RedoTransformSelection(SceneState *scene, void *raw) {
    PayloadTransformSelection *p = (PayloadTransformSelection *)raw;
    ApplyTransformEditRange(scene, p->editStart, p->editCount, true);
}
void UndoTransformSelection(SceneState *scene, void *raw) {
    PayloadTransformSelection *p = (PayloadTransformSelection *)raw;
    ApplyTransformEditRange(scene, p->editStart, p->editCount, false);
}

Transform ApplyDelta(Transform base, TransformDelta delta, Vec3 pivot) {
    Vec3 rel = Vec3Sub(base.position, pivot);
    rel = Vec3MulComponents(rel, delta.scale);
    rel = QuatRotate(delta.rotate, rel);
    Transform out;
    out.position = Vec3Add(Vec3Add(pivot, rel), delta.translate);
    out.rotation = QuatMul(delta.rotate, base.rotation);
    out.scale = Vec3MulComponents(base.scale, delta.scale);
    return out;
}

// Writes befores/afters into the side ring and pushes one undoable command.
void SubmitTransformEdits(SceneState *scene, const EntityId *ids, const Transform *befores,
                          const Transform *afters, int count) {
    if (count <= 0) {
        return;
    }
    if (count > kMaxTransformEdits) {
        count = kMaxTransformEdits;
    }
    uint32_t start = scene->transformEditHead;
    for (int i = 0; i < count; ++i) {
        TransformEdit &edit = scene->transformEdits[(start + i) % kMaxTransformEdits];
        edit.entity = ids[i];
        edit.before = befores[i];
        edit.after = afters[i];
    }
    scene->transformEditHead = (start + (uint32_t)count) % kMaxTransformEdits;

    PayloadStorage payload = {};
    payload.transformSelection.editStart = start;
    payload.transformSelection.editCount = (uint32_t)count;
    SubmitTyped(scene, SceneCmdTag_TransformSelection, RedoTransformSelection, UndoTransformSelection,
                payload);
}

int GatherSelectedEntities(const SceneState *scene, EntityId *out, int maxOut) {
    int n = 0;
    for (int i = 0; i < scene->selectionCount && n < maxOut; ++i) {
        if (scene->selection[i].kind != SelectionKind_Entity) {
            continue;
        }
        EntityId id = (EntityId)scene->selection[i].id;
        if (EntityConst(scene, id) != nullptr) {
            out[n++] = id;
        }
    }
    return n;
}

Vec3 PivotFor(const SceneState *scene, TransformDelta delta, const EntityId *ids, int count) {
    if (!delta.useCentroidPivot) {
        return delta.pivot;
    }
    Vec3 sum = Vec3{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < count; ++i) {
        sum = Vec3Add(sum, EntityWorldPos(scene, ids[i]));
    }
    return count > 0 ? Vec3Scale(sum, 1.0f / (float)count) : Vec3{0.0f, 0.0f, 0.0f};
}

} // namespace

void SceneSubmitCommand(SceneState *scene, SceneCommand command) {
    int slot = PushCommand(scene, command);
    if (scene->commands[slot].redo != nullptr) {
        scene->commands[slot].redo(scene, scene->commands[slot].payload);
    }
}

void SceneUndo(SceneState *scene) {
    if (scene->commandCursor == 0) {
        return;
    }
    --scene->commandCursor;
    SceneCommand *command = &scene->commands[(scene->commandBase + scene->commandCursor) % kMaxUndoCommands];
    if (command->undo != nullptr) {
        command->undo(scene, command->payload);
    }
}

void SceneRedo(SceneState *scene) {
    if (scene->commandCursor >= scene->commandCount) {
        return;
    }
    SceneCommand *command = &scene->commands[(scene->commandBase + scene->commandCursor) % kMaxUndoCommands];
    if (command->redo != nullptr) {
        command->redo(scene, command->payload);
    }
    ++scene->commandCursor;
}

bool SceneCanUndo(const SceneState *scene) { return scene->commandCursor > 0; }
bool SceneCanRedo(const SceneState *scene) { return scene->commandCursor < scene->commandCount; }

// ---------------------------------------------------------------------------
// Typed operations
// ---------------------------------------------------------------------------
EntityId SceneCreateEntity(SceneState *scene, EntityKind kind, const char *name) {
    PayloadStorage payload = {};
    payload.create.kind = kind;
    CopyName(payload.create.name, name);
    payload.create.result = kInvalidEntityId;
    int slot = SubmitTyped(scene, SceneCmdTag_Create, RedoCreate, UndoCreate, payload);
    return ((PayloadCreate *)scene->commands[slot].payload)->result;
}

void SceneDeleteEntity(SceneState *scene, EntityId id) {
    if (EntityConst(scene, id) == nullptr) {
        return;
    }
    PayloadStorage payload = {};
    SnapshotForDelete(scene, id, &payload.del);
    SubmitTyped(scene, SceneCmdTag_Delete, RedoDelete, UndoDelete, payload);
}

void SceneSetEntityTransform(SceneState *scene, EntityId id, Transform next) {
    const Entity *entity = EntityConst(scene, id);
    if (entity == nullptr) {
        return;
    }
    PayloadStorage payload = {};
    payload.setTransform.id = id;
    payload.setTransform.before = entity->transform;
    payload.setTransform.after = next;
    SubmitTyped(scene, SceneCmdTag_SetTransform, RedoSetTransform, UndoSetTransform, payload);
}

void SceneMakeTransformSelection(SceneState *scene, TransformDelta delta) {
    EntityId ids[kMaxEntities];
    int count = GatherSelectedEntities(scene, ids, kMaxEntities);
    if (count == 0) {
        return;
    }
    Vec3 pivot = PivotFor(scene, delta, ids, count);

    static Transform befores[kMaxEntities];
    static Transform afters[kMaxEntities];
    for (int i = 0; i < count; ++i) {
        const Entity *entity = EntityConst(scene, ids[i]);
        befores[i] = entity->transform;
        afters[i] = ApplyDelta(entity->transform, delta, pivot);
    }
    SubmitTransformEdits(scene, ids, befores, afters, count);
}

// ---------------------------------------------------------------------------
// Gizmo drag lifecycle
// ---------------------------------------------------------------------------
void SceneBeginTransformDrag(SceneState *scene) {
    scene->dragCount = GatherSelectedEntities(scene, scene->dragTargets, kMaxEntities);
    for (int i = 0; i < scene->dragCount; ++i) {
        scene->dragBase[i] = EntityConst(scene, scene->dragTargets[i])->transform;
    }
    scene->dragActive = scene->dragCount > 0;
}

void ScenePreviewTransformDrag(SceneState *scene, TransformDelta delta) {
    if (!scene->dragActive) {
        return;
    }
    Vec3 pivot = delta.pivot;
    if (delta.useCentroidPivot) {
        Vec3 sum = Vec3{0.0f, 0.0f, 0.0f};
        for (int i = 0; i < scene->dragCount; ++i) {
            sum = Vec3Add(sum, scene->dragBase[i].position);
        }
        pivot = scene->dragCount > 0 ? Vec3Scale(sum, 1.0f / (float)scene->dragCount)
                                     : Vec3{0.0f, 0.0f, 0.0f};
    }
    for (int i = 0; i < scene->dragCount; ++i) {
        Entity *entity = EntityMut(scene, scene->dragTargets[i]);
        if (entity != nullptr) {
            entity->transform = ApplyDelta(scene->dragBase[i], delta, pivot);
        }
    }
}

void SceneEndTransformDrag(SceneState *scene, TransformDelta delta) {
    if (!scene->dragActive) {
        return;
    }
    scene->dragActive = false;
    if (scene->dragCount == 0) {
        return;
    }
    Vec3 pivot = delta.pivot;
    if (delta.useCentroidPivot) {
        Vec3 sum = Vec3{0.0f, 0.0f, 0.0f};
        for (int i = 0; i < scene->dragCount; ++i) {
            sum = Vec3Add(sum, scene->dragBase[i].position);
        }
        pivot = Vec3Scale(sum, 1.0f / (float)scene->dragCount);
    }
    static Transform afters[kMaxEntities];
    for (int i = 0; i < scene->dragCount; ++i) {
        afters[i] = ApplyDelta(scene->dragBase[i], delta, pivot);
    }
    SubmitTransformEdits(scene, scene->dragTargets, scene->dragBase, afters, scene->dragCount);
}

// ---------------------------------------------------------------------------
// Direct construction (not undoable)
// ---------------------------------------------------------------------------
EntityId SceneAddEntity(SceneState *scene, EntityKind kind, const char *name, Transform transform) {
    return EntityAlloc(scene, kind, name, transform);
}

void SceneClear(SceneState *scene) {
    scene->meshRendererCount = 0;
    scene->audioSourceCount = 0;
    scene->audioListenerCount = 0;
    scene->activeListener = kInvalidEntityId;
    scene->selectionCount = 0;
    scene->activeEntity = kInvalidEntityId;
    scene->commandBase = 0;
    scene->commandCount = 0;
    scene->commandCursor = 0;
    scene->transformEditHead = 0;
    scene->dragActive = false;
    scene->dragCount = 0;

    scene->entityFreeTop = kMaxEntities;
    for (int i = 0; i < kMaxEntities; ++i) {
        scene->entityFreeStack[i] = kMaxEntities - 1 - i;
        scene->entityInUse[i] = false;
        int generation = scene->entityGeneration[i] + 1;
        if (generation >= kGenerationWrap) {
            generation = 1;
        }
        scene->entityGeneration[i] = generation;
        scene->entities[i].id = kInvalidEntityId;
    }
}

void SceneSetMesh(SceneState *scene, EntityId id, MeshId mesh, Vec3 localAabbMin, Vec3 localAabbMax) {
    Entity *entity = EntityMut(scene, id);
    if (entity == nullptr || entity->kind != EntityKind_Mesh || entity->component == kNoComponent) {
        return;
    }
    MeshRenderer &renderer = scene->meshRenderers[entity->component];
    renderer.mesh = mesh;
    renderer.localAabbMin = localAabbMin;
    renderer.localAabbMax = localAabbMax;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
SceneState *SceneInit(Arena *arena) {
    SceneState *scene = ArenaPushStruct(arena, SceneState);
    scene->arena = arena;
    scene->toolMode = ToolMode_Select;
    scene->activeListener = kInvalidEntityId;
    scene->activeEntity = kInvalidEntityId;

    scene->entityFreeTop = kMaxEntities;
    for (int i = 0; i < kMaxEntities; ++i) {
        scene->entityFreeStack[i] = kMaxEntities - 1 - i; // pop order: 0, 1, 2, ...
        scene->entityGeneration[i] = 1;
        scene->entityInUse[i] = false;
        scene->entities[i].id = kInvalidEntityId;
    }
    return scene;
}

bool SceneUpdate(SceneState *, float) { return false; }

// ---------------------------------------------------------------------------
// Iteration for renderer + audio
// ---------------------------------------------------------------------------
int SceneMeshRenderers(const SceneState *scene, SceneMeshView *out, int maxOut) {
    int n = scene->meshRendererCount < maxOut ? scene->meshRendererCount : maxOut;
    for (int i = 0; i < n; ++i) {
        const MeshRenderer &mesh = scene->meshRenderers[i];
        const Entity *entity = EntityConst(scene, mesh.entity);
        out[i].entity = mesh.entity;
        out[i].mesh = mesh.mesh;
        out[i].model = entity != nullptr ? TransformToMat4(entity->transform) : Mat4Identity();
        out[i].localAabbMin = mesh.localAabbMin;
        out[i].localAabbMax = mesh.localAabbMax;
    }
    return n;
}

int SceneAudioSources(const SceneState *scene, SceneAudioSourceView *out, int maxOut) {
    int n = scene->audioSourceCount < maxOut ? scene->audioSourceCount : maxOut;
    for (int i = 0; i < n; ++i) {
        const AudioSource &source = scene->audioSources[i];
        const Entity *entity = EntityConst(scene, source.entity);
        out[i].entity = source.entity;
        out[i].worldPos = entity != nullptr ? entity->transform.position : Vec3{0.0f, 0.0f, 0.0f};
        out[i].params = source.params;
    }
    return n;
}

bool SceneActiveListener(const SceneState *scene, Vec3 *pos, Vec3 *forward, Vec3 *up) {
    const Entity *entity = EntityConst(scene, scene->activeListener);
    if (entity == nullptr) {
        return false;
    }
    if (pos != nullptr) {
        *pos = entity->transform.position;
    }
    if (forward != nullptr) {
        *forward = QuatRotate(entity->transform.rotation, Vec3{0.0f, 0.0f, -1.0f});
    }
    if (up != nullptr) {
        *up = QuatRotate(entity->transform.rotation, Vec3{0.0f, 1.0f, 0.0f});
    }
    return true;
}

EntityId SceneActiveListenerEntity(const SceneState *scene) { return scene->activeListener; }

void SceneSetActiveListener(SceneState *scene, EntityId listenerEntity) {
    if (!EntityIdValid(listenerEntity)) {
        scene->activeListener = kInvalidEntityId;
        return;
    }
    const Entity *entity = EntityConst(scene, listenerEntity);
    if (entity != nullptr && entity->kind == EntityKind_AudioListener) {
        scene->activeListener = listenerEntity;
    }
}

const Entity *SceneGetEntity(const SceneState *scene, EntityId id) { return EntityConst(scene, id); }

bool SceneGetEntityTransform(const SceneState *scene, EntityId id, Transform *out) {
    const Entity *entity = EntityConst(scene, id);
    if (entity == nullptr) {
        return false;
    }
    *out = entity->transform;
    return true;
}

AudioSourceParams *SceneGetAudioSourceParams(SceneState *scene, EntityId id) {
    Entity *entity = EntityMut(scene, id);
    if (entity == nullptr || entity->kind != EntityKind_AudioSource ||
        entity->component == kNoComponent) {
        return nullptr;
    }
    return &scene->audioSources[entity->component].params;
}

// ---------------------------------------------------------------------------
// Tool mode
// ---------------------------------------------------------------------------
ToolMode SceneToolMode(const SceneState *scene) { return (ToolMode)scene->toolMode; }
void SceneSetToolMode(SceneState *scene, ToolMode mode) { scene->toolMode = (int)mode; }
int *SceneToolModePtr(SceneState *scene) { return &scene->toolMode; }
