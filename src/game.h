#pragma once

// Platform-agnostic scene setup. This file must not include any platform or
// rendering API headers (no Metal, no AppKit).

struct SceneState;

// Populates a fresh SceneState with the default spatial-audio test scene:
// ground plane, two static occluder boxes, two audio sources at distinct
// positions, one active listener at the origin. Direct (non-undoable)
// construction, so the undo stack starts empty.
void GameLoadDefaultScene(SceneState *scene);
