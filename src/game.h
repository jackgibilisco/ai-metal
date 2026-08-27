#pragma once

// Platform-agnostic game state and update logic. This file must not include
// any platform or rendering API headers (no Metal, no AppKit).

#include "arena.h"
#include "math3d.h"

enum class Primitive { Cube, Plane };

struct SceneObject {
    Vec3 position;
    Vec3 scale;
    Mat4 rotation;

    // Euler-angle spin state. Only used by objects with a nonzero
    // rotationSpeed (the demo cubes); imported objects are static and leave
    // both at zero, with `rotation` set once at import time instead.
    Vec3 rotationEuler;
    Vec3 rotationSpeed;

    Primitive primitive;
};

// Fixed capacity so a file import never allocates from the arena — it just
// overwrites this array, which Init already sized once.
constexpr int kMaxSceneObjects = 128;

struct GameState {
    SceneObject objects[kMaxSceneObjects];
    int objectCount;
};

GameState *GameInit(Arena *arena);
void GameUpdate(GameState *state, float deltaTime);

// Replaces the current scene with the cube/plane objects found in a Blender
// 5.x .blend file. Returns false (leaving the scene unchanged) if the file
// can't be read or contains no cube/plane mesh objects.
bool GameImportBlendFile(GameState *state, const char *filepath);
