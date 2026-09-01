#pragma once

// The renderer contract, free of any graphics API. renderer_metal.mm is the
// Metal implementation; a Windows port adds renderer_d3d12.cpp against this
// same header and leaves game.cpp and this file untouched. The backend may
// use its API freely but must not touch AppKit/UIKit or any OS windowing
// API — the platform layer owns the window and hands rendering targets in.

#include "arena.h"
#include "frame_input.h"
#include "gizmo.h"
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

// Viewport ray under a drawable-pixel point with a TOP-LEFT origin (the space
// FrameInput.mouseX/mouseY are already in). The point is mapped through the
// content rect, so panel chrome does not shift the ray. Origin is the camera
// eye; dir is normalized.
Ray RendererScreenPointToRay(const RendererState *renderer, float screenX, float screenY);

// Current view-projection, for box-select's NDC frustum test.
Mat4 RendererViewProjection(const RendererState *renderer);

// GizmoWorldScale for `pivot` against the live camera and content viewport.
// Hit-testing and drawing both go through this so the picked handle is always
// the drawn one.
float RendererGizmoScale(const RendererState *renderer, Vec3 pivot);

// What the renderer needs beyond the scene itself to draw one frame: the
// gizmo overlay state the app tracks across frames.
struct RendererSceneView {
    const SceneState *scene;
    ToolMode toolMode;
    bool gizmoVisible;
    Vec3 gizmoPivot;
    GizmoHandle hoveredHandle;
    GizmoHandle activeHandle;
};

void RendererRender(RendererState *renderer, const RendererSceneView *view, RenderTarget *target);
RendererPassTimings RendererLastFrameTimings(const RendererState *renderer);
