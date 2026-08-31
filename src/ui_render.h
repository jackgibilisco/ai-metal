#pragma once

// The UI renderer contract, free of any graphics API. ui_render_metal.mm is
// the Metal implementation: it turns the UiVertex triangle list into one
// alpha-blended pass drawn on top of the drawable. A Windows port adds a
// parallel ui_render_d3d12.cpp and leaves ui.cpp untouched.

#include "arena.h"
#include "gpu.h"
#include "ui.h"

struct UiRenderState;

UiRenderState *UiRenderInit(Arena *arena, GpuContext *gpu);
void UiRenderEncode(UiRenderState *uiRender, RenderTarget *target, const UiVertex *vertices,
                    int vertexCount, float drawableWidth, float drawableHeight);
