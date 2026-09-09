#pragma once

// The single color palette for the whole editor. Every draw site - UI panels,
// menu strip, timeline, viewport toolbar, renderer clear colors, gizmos and
// scene icons - reads its color from here. No color hex / MTLClearColor lives
// anywhere else. There is no runtime color picker in v1: this header is the one
// edit point.
//
// Pure C++, no Metal / AppKit. `Color` is 8-bit RGBA (what the UI vertex list
// wants). `ToFloat` converts to 0..1 floats for renderer clear colors and for
// the shader-source #define prelude the renderer builds from these names.

namespace theme {

struct Color {
    unsigned char r, g, b, a;
};

struct Rgba {
    float r, g, b, a;
};

constexpr Rgba ToFloat(Color c) {
    return Rgba{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
}

// ---- panels and controls (src/ui.cpp) --------------------------------------
constexpr Color PanelBg = {28, 30, 34, 255};
constexpr Color PanelBorder = {62, 66, 75, 255};
constexpr Color TitleBar = {40, 44, 51, 255};
constexpr Color TitleBarHot = {54, 59, 69, 255};
constexpr Color ResizeGrip = {58, 62, 70, 255};
constexpr Color ResizeGripHot = {92, 142, 222, 255};
constexpr Color DragOutline = {96, 150, 232, 255};
constexpr Color MarqueeFill = {96, 150, 232, 46}; // box-select drag rectangle
constexpr Color Button = {52, 57, 66, 255};
constexpr Color ButtonHot = {70, 77, 90, 255};
constexpr Color ButtonActive = {92, 142, 222, 255};
constexpr Color SliderTrack = {42, 46, 53, 255};
constexpr Color SliderHandle = {126, 174, 236, 255};
constexpr Color SliderHandleHot = {170, 202, 244, 255};
constexpr Color Text = {224, 227, 232, 255};
constexpr Color TextDisabled = {120, 124, 130, 255};
constexpr Color TextShortcut = {148, 152, 158, 255};

// ---- menu strip (src/ui.cpp) ---------------------------------------------
constexpr Color MenuBarBg = {22, 24, 27, 255};
constexpr Color MenuTitleHot = {70, 77, 90, 255};
constexpr Color MenuTitleOpen = {92, 142, 222, 255};
constexpr Color MenuDropdownBg = {38, 41, 46, 255};
constexpr Color MenuItemHot = {70, 77, 90, 255};
constexpr Color MenuSeparator = {70, 77, 90, 255};

// ---- viewport / renderer (src/renderer_metal.mm - wired by app.cpp via a
// shader-source #define prelude built from ToFloat) -----------------------
constexpr Color ViewportBackground = {13, 13, 20, 255}; // void behind geometry
constexpr Color GBufferClear = {0, 0, 0, 255};          // geometry-pass clear
constexpr Color MeshBase = {226, 228, 233, 255};        // unselected mesh (white)
constexpr Color MeshSelected = {74, 140, 242, 255};     // selected mesh (blue)

// ---- gizmos and scene icons (src/renderer_metal.mm - the renderer reads these
// names directly in the gizmo / icon pass) --------------------------------
constexpr Color GizmoAxisX = {230, 72, 72, 255};
constexpr Color GizmoAxisY = {96, 200, 88, 255};
constexpr Color GizmoAxisZ = {72, 124, 232, 255};
constexpr Color GizmoActive = {255, 214, 92, 255}; // hovered / dragged handle
constexpr Color AudioSourceIcon = {235, 170, 60, 255};
constexpr Color ListenerIcon = {150, 160, 170, 255};
constexpr Color ListenerActive = {92, 200, 160, 255}; // the one active listener
constexpr Color SelectionOutline = {255, 180, 40, 255};
constexpr Color AudioDistanceSphere = {120, 180, 220, 90}; // min/max distance wireframe

// ---- timeline panel (src/ui.cpp) ---------------------------------------
constexpr Color TimelineRuler = {34, 37, 43, 255};
constexpr Color TimelineRulerTick = {90, 96, 105, 255};
constexpr Color TimelineLaneEven = {26, 28, 32, 255};
constexpr Color TimelineLaneOdd = {30, 33, 38, 255};
constexpr Color TimelineLaneHeader = {40, 44, 51, 255};
constexpr Color TimelineClip = {74, 120, 180, 255};
constexpr Color TimelineClipSelected = {120, 170, 236, 255};
constexpr Color TimelineClipGhost = {120, 170, 236, 90}; // drag-hover drop preview
constexpr Color TimelineClipTrimHandle = {206, 224, 248, 255};
constexpr Color Playhead = {236, 196, 92, 255};

// ---- 3D-editor toolbar (src/ui.cpp) ----------------------------------
constexpr Color ToolbarBg = {24, 26, 30, 220};
constexpr Color ToolButton = {52, 57, 66, 255};
constexpr Color ToolButtonHot = {70, 77, 90, 255};
constexpr Color ToolButtonActive = {92, 142, 222, 255}; // current tool mode

} // namespace theme
