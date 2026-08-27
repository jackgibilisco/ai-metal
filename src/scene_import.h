#pragma once

// Imports Blender .blend files into GameState. Platform-agnostic like
// game.h, but split out because it depends on blend_file.h.

#include "game.h"

// Replaces the current scene with the cube/plane objects found in a Blender
// 5.x .blend file. Returns false (leaving the scene unchanged) if the file
// can't be read or contains no cube/plane mesh objects.
bool SceneImportBlendFile(GameState *state, const char *filepath);
