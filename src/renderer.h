#pragma once

// The renderer contract, free of any graphics API. renderer_metal.mm is the
// Metal implementation; a Windows port adds renderer_d3d12.cpp against this
// same header and leaves game.cpp and this file untouched. The backend may
// use its API freely but must not touch AppKit/UIKit or any OS windowing
// API — the platform layer owns the window and hands rendering targets in.

#include "arena.h"
#include "frame_input.h"
#include "gpu.h"
#include "scene.h"

struct RendererState;

// GPU time in milliseconds for each pass of the last completed frame, for
// the F3 HUD. A pass that didn't run that frame reads 0. Per-pass values
// assume Apple-silicon nanosecond timestamps; total is measured
// independently from the command buffer's GPU start/end.
struct RendererPassTimings {
    float geometryMs;
    float aoMs;
    float lightingMs;
    float fxaaMs;
    float totalMs;
};

RendererState *RendererInit(Arena *arena, GpuContext *gpu, float drawableWidth,
                            float drawableHeight);

// The scene renders into an `(originX, originY, width, height)` region of the
// drawable (the part the UI panels and menu strip don't take). Sets the
// projection aspect and rebuilds the screen-sized targets when the size
// changes; the origin only shifts where the final pass writes the drawable.
void RendererSetContentRect(RendererState *renderer, float originX, float originY, float width,
                            float height);
void RendererUpdateCamera(RendererState *renderer, FrameInput input);

// The orbit camera's focus point (world space) — where new entities drop when
// the placement ray has no better hit.
Vec3 RendererCameraFocus(const RendererState *renderer);
void RendererRender(RendererState *renderer, const SceneState *scene, RenderTarget *target);
RendererPassTimings RendererLastFrameTimings(const RendererState *renderer);
