#include "game.h"

GameState *GameInit(Arena *arena) {
    GameState *state = ArenaPushStruct(arena, GameState);

    for (int i = 0; i < kCubeCount; ++i) {
        state->cubes[i].position = Vec3{(i - 1) * 3.0f, 0.0f, 0.0f};
        state->cubes[i].rotation = Vec3{0.0f, 0.0f, 0.0f};
    }

    return state;
}

void GameUpdate(GameState *state, float deltaTime) {
    for (int i = 0; i < kCubeCount; ++i) {
        Cube *cube = &state->cubes[i];
        cube->rotation.x += deltaTime * (0.6f + 0.2f * i);
        cube->rotation.y += deltaTime * (0.9f - 0.15f * i);
    }
}
