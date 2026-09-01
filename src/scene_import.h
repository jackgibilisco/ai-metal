#pragma once

// Imports Blender .blend files into a SceneState. Platform-agnostic like
// game.h, but split out because it depends on blend_file.h.

struct SceneState;

// Replaces the current scene with the cube/plane objects found in a Blender
// 5.x .blend file. Returns false (leaving the scene unchanged) if the file
// can't be read or contains no cube/plane mesh objects. Non-undoable: the
// scene is cleared and the undo history reset.
bool SceneImportBlendFile(SceneState *scene, const char *filepath);
