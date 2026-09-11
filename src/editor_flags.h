#pragma once

// The editor's debug/view toggles as plain data. Every one of them is reachable
// from the command table (Debug and Window menus, F3 chords), so the flags live
// in one struct the command context can point at instead of being scattered
// across the renderer, the UI and the platform layer.

struct EditorFlags {
    int aoDebugView;           // 0 normal, 1 raw AO buffer, 2 AO disabled
    bool fxaaEnabled;
    bool layoutBounds;         // magenta outlines on every UI container
    bool frameStatsHud;
    bool resetPanelsRequested; // one-shot; app.cpp clears it after resetting
};
