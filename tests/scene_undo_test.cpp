// Characterization test for the scene command / undo-redo stack. Headless, no
// Metal. Pins current behavior before the undo stack is split into its own
// module.
//
// Build + run:
//   clang++ -std=c++17 -Wall -Wextra -I src tests/scene_undo_test.cpp \
//     src/scene.cpp src/arena.cpp -o build/scene_undo_test && build/scene_undo_test

#include <cstdio>
#include <cstdlib>

#include "scene.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static SceneState *MakeScene() {
    size_t bytes = 64 * 1024 * 1024;
    static Arena arena = ArenaCreate(malloc(bytes), bytes);
    return SceneInit(&arena);
}

static int MeshCount(const SceneState *scene) {
    static SceneMeshView views[kMaxMeshRenderers];
    return SceneMeshRenderers(scene, views, kMaxMeshRenderers);
}

static void TestCreateUndoRedo() {
    SceneState *scene = MakeScene();
    CHECK(!SceneCanUndo(scene));

    EntityId id = SceneCreateEntity(scene, EntityKind_Mesh, "cube");
    CHECK(EntityIdValid(id));
    CHECK(MeshCount(scene) == 1);
    CHECK(SceneCanUndo(scene));
    CHECK(!SceneCanRedo(scene));

    SceneUndo(scene);
    CHECK(MeshCount(scene) == 0);
    CHECK(!SceneCanUndo(scene));
    CHECK(SceneCanRedo(scene));

    SceneRedo(scene);
    CHECK(MeshCount(scene) == 1);
    CHECK(SceneCanUndo(scene));
}

static void TestTransformUndoRedo() {
    SceneState *scene = MakeScene();
    EntityId id = SceneCreateEntity(scene, EntityKind_Mesh, "cube");

    Transform moved = TransformIdentity();
    moved.position = Vec3{3.0f, 0.0f, 0.0f};
    SceneSetEntityTransform(scene, id, moved);

    Transform got;
    CHECK(SceneFindEntityTransform(scene, id, &got));
    CHECK(got.position.x == 3.0f);

    SceneUndo(scene);
    CHECK(SceneFindEntityTransform(scene, id, &got));
    CHECK(got.position.x == 0.0f);

    SceneRedo(scene);
    CHECK(SceneFindEntityTransform(scene, id, &got));
    CHECK(got.position.x == 3.0f);
}

static void TestDeleteUndoRestoresComponent() {
    SceneState *scene = MakeScene();
    EntityId id = SceneCreateEntity(scene, EntityKind_Mesh, "cube");
    CHECK(MeshCount(scene) == 1);

    SceneDeleteEntity(scene, id);
    CHECK(MeshCount(scene) == 0);
    CHECK(SceneFindEntity(scene, id) == nullptr);

    SceneUndo(scene);
    CHECK(MeshCount(scene) == 1);
    CHECK(SceneFindEntity(scene, id) != nullptr);
}

static void TestNewCommandDropsRedo() {
    SceneState *scene = MakeScene();
    SceneCreateEntity(scene, EntityKind_Mesh, "a");
    SceneUndo(scene);
    CHECK(SceneCanRedo(scene));

    SceneCreateEntity(scene, EntityKind_Mesh, "b");
    CHECK(!SceneCanRedo(scene));
    CHECK(MeshCount(scene) == 1);
}

int main() {
    TestCreateUndoRedo();
    TestTransformUndoRedo();
    TestDeleteUndoRestoresComponent();
    TestNewCommandDropsRedo();
    if (g_failures == 0) {
        std::printf("scene_undo_test: all checks passed\n");
        return 0;
    }
    std::printf("scene_undo_test: %d check(s) failed\n", g_failures);
    return 1;
}
