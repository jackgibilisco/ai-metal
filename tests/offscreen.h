#pragma once

// Headless render target for tests/render_parity.cpp. One implementation per
// graphics backend (offscreen_metal.mm, offscreen_gl.mm); the parity binary
// links exactly one of them alongside the matching renderer backend.

#include <cstdint>

#include "gpu.h"

GpuContext *OffscreenInit(int width, int height);
void OffscreenResize(int width, int height);
// Clears the color target to magenta so pixels no pass touches are visible.
RenderTarget *OffscreenBeginFrame();
// Waits for the GPU, then copies the frame out as tightly packed RGBA8 rows,
// top row first. Pass nullptr to only wait.
void OffscreenEndFrame(uint8_t *rgbaTopLeft);
