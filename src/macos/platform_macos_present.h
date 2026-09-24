#pragma once

// The graphics-API half of the macOS platform layer. platform_macos.mm owns
// the window, input, menus, and HUD and never names Metal or GL; exactly one
// presenter is linked beside it (platform_macos_metal.mm or
// platform_macos_gl.mm) and owns the drawable surface, the display-refresh
// frame callback, and presentation.

#import <Cocoa/Cocoa.h>

#include "gpu.h"

struct Presenter;

// Runs once per display refresh, on the main thread, while running.
typedef void (*PresenterFrameCallback)(void *context);

// For the content view's -makeBackingLayer; nil keeps AppKit's default layer.
CALayer *PresenterMakeBackingLayer();

// `view` is the window's content view, already backed by its layer.
Presenter *PresenterCreate(NSView *view, PresenterFrameCallback callback, void *context);
GpuContext *PresenterGpuContext(Presenter *presenter);
void PresenterResize(Presenter *presenter, CGSize backingSize);

// Starts the frame callback; call once the view is on screen.
void PresenterStart(Presenter *presenter);
void PresenterSetRunning(Presenter *presenter, bool running);
bool PresenterRunning(const Presenter *presenter);
// Paces frames to `screen` (re-applied when the window changes screens).
void PresenterMatchScreen(Presenter *presenter, NSScreen *screen, int framesPerSecond);

// Only inside the frame callback. Begin returns this refresh's target, or
// nullptr when there is nothing to draw into; End presents it.
RenderTarget *PresenterBeginFrame(Presenter *presenter);
void PresenterEndFrame(Presenter *presenter);
