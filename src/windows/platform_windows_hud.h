#pragma once

// The Windows frame-timing HUD: a click-through overlay window pinned over the
// top-left of the main window's client area. The counterpart to macOS's
// DebugHudView, and like it, it owns its own FrameStats and is fed one sample
// per rendered frame.

#include <windows.h>

#include "renderer.h"

struct DebugHud;

DebugHud *HudCreate(HWND owner);
void HudSetVisible(DebugHud *hud, bool visible);
bool HudVisible(const DebugHud *hud);
HWND HudWindow(const DebugHud *hud);

// One display refresh in milliseconds; sets the graph scale and the Hz readout.
void HudSetTargetFrameMs(DebugHud *hud, float targetFrameMs);
void HudPushFrameTime(DebugHud *hud, float deltaSeconds);
void HudSetPassTimings(DebugHud *hud, RendererPassTimings timings);

// Re-pins the overlay after the owner window moves or resizes.
void HudFollowOwner(DebugHud *hud);
