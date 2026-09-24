#pragma once

// The graphics-API half of the Windows platform layer. platform_windows.cpp
// owns the window, input, menus, and HUD and never names GL; the presenter
// owns the rendering context, the frame target, and presentation. Same split
// as platform_macos_present.h, minus the display link: on Windows a swap
// interval of 1 paces the frames that render, and PresenterWaitVBlank paces
// the frames the demand-driven loop skips.

#include <windows.h>

#include "gpu.h"

struct Presenter;

// Makes the context current; it stays current for the life of the process, so
// Init can build the renderer's GL objects straight after this returns.
Presenter *PresenterCreate(HWND window);
GpuContext *PresenterGpuContext(Presenter *presenter);
void PresenterResize(Presenter *presenter, int width, int height);

// The target to draw this frame into; End presents it, blocking until the
// next vertical blank.
RenderTarget *PresenterBeginFrame(Presenter *presenter);
void PresenterEndFrame(Presenter *presenter);

// Blocks until the next vertical blank without presenting, so an idle frame
// costs the same wall time as a rendered one.
void PresenterWaitVBlank(Presenter *presenter);

// Refresh rate of the monitor the window is on, for the HUD's target line.
int PresenterDisplayRefreshHz(HWND window);
