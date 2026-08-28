#pragma once

// One frame's worth of raw input from the platform layer. Pure C++ so both
// the renderer (camera) and the UI core can share it without pulling in any
// platform or graphics headers.

struct FrameInput {
    // Accumulated pointer deltas since the last frame. Plain points/units —
    // the renderer decides how to interpret them.
    float panX;        // two-finger drag or right-drag
    float panY;        // two-finger drag or right-drag
    float zoomDelta;   // pinch magnification or mouse wheel
    float orbitYaw;    // shift + two-finger drag, shift-right-drag, or middle-drag
    float orbitPitch;  // shift + two-finger drag, shift-right-drag, or middle-drag
    bool cycleDebugView; // one keypress: advance the AO debug view mode
    bool toggleFxaa;     // one keypress: enable/disable the FXAA post pass

    // Absolute cursor state for the UI, in backing pixels with a top-left
    // origin (same space the renderer's drawable uses).
    float mouseX;
    float mouseY;
    bool mouseDown; // left button currently held

    bool fullscreen; // borderless fullscreen: the in-app menu strip is forced on
};
