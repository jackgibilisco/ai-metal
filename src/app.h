#pragma once

// The exact API the platform layer calls: Init once, then FrameUpdate and
// FrameRender every frame. Everything the program needs lives in the arena
// passed to each call; nothing else is allocated after Init returns.
//
// A platform layer reports raw input and window events and nothing more:
// what a key, drag, or dropped file means is decided here.

#include "arena.h"
#include "frame_input.h"
#include "gpu.h"
#include "menu.h"
#include "renderer.h"

void Init(Arena *arena, GpuContext *gpu, float drawableWidth, float drawableHeight,
          PlatformMenuHooks menuHooks);

// Returns true if the frame changed something the renderer would draw
// differently. When it returns false the platform layer may skip FrameRender
// and leave the last presented frame on screen. Pass deltaTime 0 on the first
// frame after the loop starts or resumes from a pause: the app then forces a
// few rendered frames so multi-buffered targets flush to the screen.
bool FrameUpdate(Arena *arena, float deltaTime, FrameInput input);
void FrameRender(Arena *arena, RenderTarget *target);
void FrameResize(Arena *arena, float drawableWidth, float drawableHeight);

// A native menu bar item was chosen, or needs its enabled / checked state to
// draw itself. Both resolve through the shared command table.
void AppInvokeCommand(Arena *arena, CommandId id);
CommandState AppCommandState(Arena *arena, CommandId id);

// Whether a file dragged over the window would be accepted if dropped, so the
// platform can show the right drag cursor.
bool AppAcceptsDroppedFile(const char *path);

// Per-pass GPU time of the last completed frame. Only the parity harness reads
// it; the frame-timing HUD is drawn by the app itself.
RendererPassTimings FrameGpuTimings(Arena *arena);
