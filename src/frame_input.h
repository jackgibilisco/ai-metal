#pragma once

// One frame's worth of input from the platform layer. Pure C++ so the
// renderer (camera) and the UI core can share it without pulling in any
// platform or graphics headers.

// Keys that produce no character. Each platform layer maps its own virtual key
// codes into this set, so app.cpp and ui.cpp never see a native key code.
enum {
    Key_None = 0,
    Key_Return,
    Key_Escape,
    Key_Backspace,
    Key_Delete,
    Key_Left,
    Key_Right,
    Key_F3,
};

struct KeyEvent {
    int keyCode;
    unsigned int codepoint; // 0 when the key produces no character
    unsigned int mods;      // bit 0 cmd, 1 shift, 2 ctrl, 3 alt (matches ShortcutMod_*)
    bool pressed;           // true = key down, false = key up
};

constexpr int kMaxKeyEvents = 16;

struct FrameInput {
    // Accumulated camera deltas since the last frame from gestures the platform
    // interprets itself (trackpad scroll, wheel, pinch). Mouse-button drags
    // arrive as mouseDeltaX/Y instead; the app maps those onto the camera.
    float panX;
    float panY;
    float zoomDelta;
    float orbitYaw;
    float orbitPitch;

    // Pointer motion since the last frame, in points (backing pixels divided by
    // the display scale), so drag speed feels the same on every display.
    float mouseDeltaX;
    float mouseDeltaY;

    // Trackpad pinch this frame, in the raw NSEvent magnification unit. Kept
    // separate from zoomDelta so the UI can claim it (timeline zoom) while the
    // camera still gets it everywhere else.
    float magnification;

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

    // Refresh rate of the display the window is on; the frame-timing HUD's
    // graph is scaled against one refresh interval. 0 when unknown.
    float displayRefreshHz;

    // A file the user picked in the open dialog (PlatformMenuHooks
    // showOpenDialog), or null. Owned by the platform, valid this frame only.
    const char *openedFile;

    // Files dropped onto the window this frame. The array and the strings it
    // points at are owned by the platform layer and valid for this frame only.
    const char *const *droppedFiles;
    int droppedFileCount;
    float dropX; // drop point in backing pixels, top-left origin
    float dropY;

    // A file drag currently held over the window, before any drop. Valid every
    // frame the drag is inside; dragHovering is false otherwise. Lets the
    // timeline draw a ghost of where a dropped clip would land.
    bool dragHovering;
    float dragHoverX; // backing pixels, top-left origin
    float dragHoverY;
    int dragHoverFileCount;
};
