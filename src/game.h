#pragma once

// Platform-agnostic game state and update logic. This file must not include
// any platform or rendering API headers (no Metal, no AppKit).

#include "arena.h"
#include "math3d.h"

constexpr int kCubeCount = 3;

struct Cube {
    Vec3 position;
    Vec3 rotation;
};

struct GameState {
    Cube cubes[kCubeCount];
};

GameState *GameInit(Arena *arena);
void GameUpdate(GameState *state, float deltaTime);
