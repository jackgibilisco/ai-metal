#include "game.h"

#include "scene.h"

void GameLoadDefaultScene(SceneState *scene) {
    Transform transform = TransformIdentity();

    transform.position = Vec3{0.0f, 0.0f, 0.0f};
    transform.scale = Vec3{20.0f, 1.0f, 20.0f};
    EntityId ground = SceneAddEntity(scene, EntityKind_Mesh, "Ground", transform);
    SceneSetMesh(scene, ground, MeshId_Plane, Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 0.0f, 0.5f});

    transform = TransformIdentity();
    transform.position = Vec3{-3.0f, 1.0f, -2.0f};
    transform.scale = Vec3{2.0f, 2.0f, 2.0f};
    EntityId occluderA = SceneAddEntity(scene, EntityKind_Mesh, "Occluder A", transform);
    SceneSetMesh(scene, occluderA, MeshId_Cube, Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f});

    transform = TransformIdentity();
    transform.position = Vec3{4.0f, 1.5f, 1.0f};
    transform.scale = Vec3{1.5f, 3.0f, 1.5f};
    EntityId occluderB = SceneAddEntity(scene, EntityKind_Mesh, "Occluder B", transform);
    SceneSetMesh(scene, occluderB, MeshId_Cube, Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f});

    transform = TransformIdentity();
    transform.position = Vec3{-6.0f, 1.0f, 4.0f};
    SceneAddEntity(scene, EntityKind_AudioSource, "Source Left", transform);

    transform = TransformIdentity();
    transform.position = Vec3{7.0f, 1.0f, -5.0f};
    SceneAddEntity(scene, EntityKind_AudioSource, "Source Right", transform);

    transform = TransformIdentity();
    transform.position = Vec3{0.0f, 1.0f, 0.0f};
    EntityId listener = SceneAddEntity(scene, EntityKind_AudioListener, "Listener", transform);
    SceneSetActiveListener(scene, listener);
}
