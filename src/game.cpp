#include "game.h"

#include "scene_import.h"

namespace {

constexpr int kDemoCubeCount = 3;

} // namespace

GameState *GameInit(Arena *arena) {
    GameState *state = ArenaPushStruct(arena, GameState);

    state->objectCount = kDemoCubeCount;
    for (int i = 0; i < kDemoCubeCount; ++i) {
        SceneObject &object = state->objects[i];
        object.position = Vec3{(i - 1) * 3.0f, 0.0f, 0.0f};
        object.scale = Vec3{1.0f, 1.0f, 1.0f};
        object.rotation = Mat4Identity();
        object.rotationEuler = Vec3{0.0f, 0.0f, 0.0f};
        object.rotationSpeed = Vec3{0.6f + 0.2f * i, 0.9f - 0.15f * i, 0.0f};
        object.primitive = Primitive::Cube;
    }

    return state;
}

void GameUpdate(GameState *state, float deltaTime) {
    for (int i = 0; i < state->objectCount; ++i) {
        SceneObject &object = state->objects[i];
        bool spins = object.rotationSpeed.x != 0.0f || object.rotationSpeed.y != 0.0f ||
                     object.rotationSpeed.z != 0.0f;
        if (!spins) {
            continue;
        }
        object.rotationEuler.x += object.rotationSpeed.x * deltaTime;
        object.rotationEuler.y += object.rotationSpeed.y * deltaTime;
        object.rotationEuler.z += object.rotationSpeed.z * deltaTime;
        object.rotation = Mat4EulerXYZ(object.rotationEuler);
    }
}

bool GameImportBlendFile(GameState *state, const char *filepath) {
    return SceneImportBlendFile(state, filepath);
}
