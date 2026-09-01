#pragma once

// One frame's worth of input from the platform layer. Pure C++ so the
// renderer (camera) and the UI core can share it without pulling in any
// platform or graphics headers.

struct KeyEvent {
    int keyCode;
    unsigned int codepoint; // 0 when the key produces no character
    unsigned int mods;      // bit 0 cmd, 1 shift, 2 ctrl, 3 alt (matches ShortcutMod_*)
    bool pressed;           // true = key down, false = key up
};

constexpr int kMaxKeyEvents = 16;

struct FrameInput {
    // Accumulated camera pointer deltas since the last frame.
    float panX;
    float panY;
    float zoomDelta;
    float orbitYaw;
    float orbitPitch;
    bool cycleDebugView; // one keypress: advance the AO debug view mode
    bool toggleFxaa;     // one keypress: enable/disable the FXAA post pass

    // Cursor in backing pixels with a top-left origin (the drawable's space).
    float mouseX;
    float mouseY;
    bool mouseLeftDown;
    bool mouseRightDown;
    bool mouseMiddleDown;

    // Scroll for the UI, accumulated per frame. Separate from the camera
    // zoom/pan deltas above.
    float scrollX;
    float scrollY;

    bool shift;
    bool ctrl;
    bool alt;
    bool cmd;

    KeyEvent keyEvents[kMaxKeyEvents]; // overflow past kMaxKeyEvents is dropped
    int keyEventCount;

    bool fullscreen; // borderless fullscreen: the in-app menu strip is forced on

    // Files dropped onto the window this frame. The array and the strings it
    // points at are owned by the platform layer and valid for this frame only.
    const char *const *droppedFiles;
    int droppedFileCount;
    float dropX; // drop point in backing pixels, top-left origin
    float dropY;
};
