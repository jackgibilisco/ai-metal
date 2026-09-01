#pragma once

// The editor's scene representation: fixed-capacity entity + component pools
// in the arena, a shared entity/clip selection set, ray/AABB picking, and a
// unified command/undo stack. Pure C++: no Metal, no AppKit. Renderer and
// audio read the scene through the iteration helpers near the bottom; Remus's
// gizmo and Dan's timeline push edits through SceneSubmitCommand.

#include <cstdint>

#include "arena.h"
#include "audio.h" // AudioSourceParams, embedded by value in the AudioSource component
#include "math3d.h"

// ---------------------------------------------------------------------------
// Capacities (locked in docs/scene-editor-plan.md; manager sign-off to change)
// ---------------------------------------------------------------------------
constexpr int kMaxEntities = 4096;
constexpr int kMaxMeshRenderers = 4096;
constexpr int kMaxAudioSources = 256;
constexpr int kMaxAudioListeners = 16;
constexpr int kMaxUndoCommands = 1024;    // ring buffer; oldest dropped on overflow
constexpr int kMaxSelection = 5120;       // entities (4096) + clips (1024), combined
constexpr int kMaxTransformEdits = 16384; // side ring backing multi-select transform undo

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
// int32_t so it drops straight into the manager's / Andy's `int entity`
// fields. Packed [30:12] generation | [11:0] slot. 0 is always invalid; a
// live id has generation >= 1 so it is never 0. Compare by equality only.
typedef int32_t EntityId;
constexpr EntityId kInvalidEntityId = 0;
inline bool EntityIdValid(EntityId id) { return id != 0; }

enum EntityKind {
    EntityKind_Mesh,
    EntityKind_AudioSource,
    EntityKind_AudioListener,
};

// Which built-in mesh a MeshRenderer draws. The renderer maps these to its
// vertex/index buffers. No asset pipeline in v1.
enum MeshId {
    MeshId_Cube,
    MeshId_Plane,
};

// Unscoped, underlying type int: Dan's toolbar writes through an `int*` alias,
// Remus's gizmo reads it.
enum ToolMode {
    ToolMode_Select,
    ToolMode_Translate,
    ToolMode_Rotate,
    ToolMode_Scale,
};

// ---------------------------------------------------------------------------
// Transform + entity
// ---------------------------------------------------------------------------
struct Transform {
    Vec3 position;
    Quat rotation;
    Vec3 scale;
};

inline Transform TransformIdentity() {
    return Transform{Vec3{0.0f, 0.0f, 0.0f}, QuatIdentity(), Vec3{1.0f, 1.0f, 1.0f}};
}

inline Mat4 TransformToMat4(const Transform &t) {
    Mat4 translation = Mat4Translation(t.position);
    Mat4 rotation = QuatToMat4(t.rotation);
    Mat4 scale = Mat4Scale(t.scale);
    return Mat4Multiply(translation, Mat4Multiply(rotation, scale));
}

struct Entity {
    EntityId id;
    char name[64];
    Transform transform;
    EntityKind kind;
    uint32_t component; // dense index into the pool for `kind`; kept valid across swap-remove
};

// ---------------------------------------------------------------------------
// Component pools (one dense array per kind)
// ---------------------------------------------------------------------------
struct MeshRenderer {
    EntityId entity;
    MeshId mesh;
    Vec3 localAabbMin; // object-space bounds, before the entity transform
    Vec3 localAabbMax;
};

struct AudioSource {
    EntityId entity;
    AudioSourceParams params; // by value
};

struct AudioListener {
    EntityId entity;
};

// ---------------------------------------------------------------------------
// Selection (shared across viewport + timeline; never on the undo stack)
// ---------------------------------------------------------------------------
enum SelectionKind {
    SelectionKind_Entity,
    SelectionKind_Clip, // `id` is an opaque timeline-clip handle owned by Dan
};

struct SelectionItem {
    SelectionKind kind;
    uint32_t id; // EntityId bits for entities; opaque handle for clips
};

inline bool SelectionItemEqual(SelectionItem a, SelectionItem b) {
    return a.kind == b.kind && a.id == b.id;
}

struct Selection {
    const SelectionItem *items;
    int count;
    EntityId activeEntity; // primary entity (gizmo anchor); kInvalidEntityId if none
};

// ---------------------------------------------------------------------------
// Picking (Remus: gizmo/click pick + placement; Andy: listener->source occlusion)
// ---------------------------------------------------------------------------
struct Ray {
    Vec3 origin;
    Vec3 dir; // expected normalized
};

struct PickResult {
    float distance;
    EntityId entity; // int32_t
    Vec3 hitPoint;
};

// ---------------------------------------------------------------------------
// Commands / undo — one generic variant everyone shares
// ---------------------------------------------------------------------------
// The single command shape everyone shares. Dan's timeline ops and any other
// module wrap their edit as `redo`/`undo` bodies over an opaque `payload` the
// submitter allocates from the arena (session-only; an evicted command's
// payload is simply forgotten, the undo ring holds 1024). Eric does not
// extend a per-request enum. `tag` is informational only (inspection /
// debugging); keep your own tags >= SceneCmdTag_UserBase.
enum { SceneCmdTag_Internal = 0, SceneCmdTag_UserBase = 1000 };

struct SceneCommand {
    int tag;
    void *payload;
    void (*redo)(SceneState *scene, void *payload);
    void (*undo)(SceneState *scene, void *payload);
};

struct SceneState; // opaque; defined in scene.cpp

void SceneSubmitCommand(SceneState *scene, SceneCommand command); // runs redo + pushes
void SceneUndo(SceneState *scene);
void SceneRedo(SceneState *scene);
bool SceneCanUndo(const SceneState *scene);
bool SceneCanRedo(const SceneState *scene);

// ---------------------------------------------------------------------------
// Typed operations (build a SceneCommand internally + submit; one undo step each)
// ---------------------------------------------------------------------------
// Payloads come from a fixed scene-owned pool, not unbounded arena pushes.
EntityId SceneCreateEntity(SceneState *scene, EntityKind kind, const char *name);
void SceneDeleteEntity(SceneState *scene, EntityId id);
void SceneSetEntityTransform(SceneState *scene, EntityId id, Transform next);

// Multi-select transform. Applies `delta` about a pivot to every selected
// entity as ONE undoable command (one transform-edit range). Pivot = the
// selection centroid (mean of selected entity world positions) unless
// `useCentroidPivot` is false, then `pivot` is used. Purpose-built: gizmo
// drags and non-drag callers both go through this, never the generic SceneCommand.
struct TransformDelta {
    Vec3 translate;
    Quat rotate;
    Vec3 scale; // component-wise multiplier; {1,1,1} = no change
    Vec3 pivot;
    bool useCentroidPivot;
};
void SceneMakeTransformSelection(SceneState *scene, TransformDelta delta); // build + submit

// Gizmo drag lifecycle: Begin snapshots every selected entity's transform;
// Preview re-applies `delta` from that snapshot each frame with NO undo push
// (live viewport preview); End re-applies and pushes exactly ONE command
// covering the whole selection (same semantics as SceneMakeTransformSelection).
// Calling End or Preview without a prior Begin is a no-op.
void SceneBeginTransformDrag(SceneState *scene);
void ScenePreviewTransformDrag(SceneState *scene, TransformDelta delta);
void SceneEndTransformDrag(SceneState *scene, TransformDelta delta);

// ---------------------------------------------------------------------------
// Direct construction (NOT undoable) — for scene loading (GameLoadDefaultScene)
// ---------------------------------------------------------------------------
// Creates an entity + its component pool slot without touching the undo
// stack, so a freshly loaded scene starts with an empty history.
EntityId SceneAddEntity(SceneState *scene, EntityKind kind, const char *name, Transform transform);
// Overrides a mesh entity's mesh id + local AABB (default is a unit cube).
void SceneSetMesh(SceneState *scene, EntityId id, MeshId mesh, Vec3 localAabbMin,
                  Vec3 localAabbMax);

// Wipes all entities, components, selection, and the undo history. For a
// full-scene replacement (GameLoadDefaultScene, .blend import).
void SceneClear(SceneState *scene);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
SceneState *SceneInit(Arena *arena);

// Per-frame hook. The default scene has no animation, so this returns false
// today; kept so the manager can gate it on the transport clock later.
// Returns true if anything the renderer would draw changed.
bool SceneUpdate(SceneState *scene, float simDeltaTime);

// ---------------------------------------------------------------------------
// Selection API (immediate, not undoable)
// ---------------------------------------------------------------------------
Selection SceneSelection(const SceneState *scene);
void SceneSelectionClear(SceneState *scene);
void SceneSelectionSet(SceneState *scene, const SelectionItem *items, int count);
void SceneSelectionAdd(SceneState *scene, SelectionItem item);
void SceneSelectionRemove(SceneState *scene, SelectionItem item);
void SceneSelectionToggle(SceneState *scene, SelectionItem item); // shift-click
bool SceneSelectionContains(const SceneState *scene, SelectionItem item);
Vec3 SceneSelectionCentroid(const SceneState *scene);

// Screen-rect box select. `rect` is in NDC: x/y in [-1, 1], y up. Tests each
// entity's world AABB against the sub-frustum `viewProj` carves from the rect.
// `additive` (shift held) unions into the current selection; otherwise it
// replaces. Returns how many entities the rect selected.
struct NdcRect {
    float minX, minY, maxX, maxY;
};
int SceneBoxSelect(SceneState *scene, Mat4 viewProj, NdcRect rect, bool additive);

// Non-mesh entities (audio sources, listeners) have no MeshRenderer, so they
// pick against a synthetic axis-aligned box of this half-extent centered on
// the entity origin. One pick path covers meshes and icons alike.
constexpr float kNonMeshPickHalfExtent = 0.3f;

// Nearest entity whose world AABB (or synthetic box, above) the ray hits.
// Returns false and leaves `out` untouched when nothing is hit.
bool ScenePickRay(const SceneState *scene, Ray ray, PickResult *out);

// Same, skipping `ignoreEntity` (pass kInvalidEntityId for none). This is the
// single pick path for viewport clicks, gizmo/icon selection, and Andy's
// listener->source occlusion raycast (which excludes the source).
bool ScenePickRayExcluding(const SceneState *scene, Ray ray, EntityId ignoreEntity,
                           PickResult *out);

// ---------------------------------------------------------------------------
// Iteration for renderer + audio (fill a caller buffer, return the count)
// ---------------------------------------------------------------------------
struct SceneMeshView {
    EntityId entity;
    MeshId mesh;
    Mat4 model; // world matrix from the entity transform
    Vec3 localAabbMin;
    Vec3 localAabbMax;
};
int SceneMeshRenderers(const SceneState *scene, SceneMeshView *out, int maxOut);

struct SceneAudioSourceView {
    EntityId entity;
    Vec3 worldPos;
    AudioSourceParams params;
};
int SceneAudioSources(const SceneState *scene, SceneAudioSourceView *out, int maxOut);

// Active listener (multiple allowed, exactly one active — decision 11).
// Returns false and leaves the out params untouched when there is none.
// forward = rotation * (0,0,-1), up = rotation * (0,1,0).
bool SceneActiveListener(const SceneState *scene, Vec3 *pos, Vec3 *forward, Vec3 *up);
EntityId SceneActiveListenerEntity(const SceneState *scene);
void SceneSetActiveListener(SceneState *scene, EntityId listenerEntity);

// Direct component/entity lookup by id (null / false if stale or missing).
const Entity *SceneGetEntity(const SceneState *scene, EntityId id);
bool SceneGetEntityTransform(const SceneState *scene, EntityId id, Transform *out);
AudioSourceParams *SceneGetAudioSourceParams(SceneState *scene, EntityId id); // mutable, for inspector

// ---------------------------------------------------------------------------
// Tool mode (Dan's toolbar writes, Remus's gizmo reads)
// ---------------------------------------------------------------------------
ToolMode SceneToolMode(const SceneState *scene);
void SceneSetToolMode(SceneState *scene, ToolMode mode);
int *SceneToolModePtr(SceneState *scene); // int alias for Dan's toolbar binding
