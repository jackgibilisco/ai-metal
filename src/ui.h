#pragma once

// Immediate-mode UI core. Pure C++: no Metal, no AppKit. Each frame it reads
// one FrameInput, runs the panel/control calls, and rebuilds a flat triangle
// list in screen-pixel coordinates (top-left origin). A Metal/D3D backend
// turns that list into draw calls.
//
// UiState holds view state only (panel placement/size, drag state, open menu,
// hot/active control, previous input, the vertex buffer). The UI renders
// app/game state passed in and reports intents back out; it does not own
// domain data.

#include "arena.h"
#include "frame_input.h"
#include "menu.h"

struct UiVertex {
    float x;
    float y;
    float u;
    float v;
    float mode; // 0 = solid color; 1 = textured glyph
    unsigned char rgba[4];
};

// Per-glyph placement for the proportional font atlas the UI renderer bakes
// (ASCII 32..127). Lengths are in logical UI pixels; uv is into the atlas.
struct UiGlyphMetric {
    float u0, v0, u1, v1;
    float advance;          // pen advance to the next glyph
    float offsetX, offsetY; // quad top-left relative to the pen (offsetY from the baseline, +up)
    float width, height;    // quad size (0 for whitespace)
};

struct UiFontMetrics {
    UiGlyphMetric glyphs[96];
    float ascent;    // baseline offset from the top of a text line
    float descent;   // baseline to bottom of the line (positive)
    float lineHeight; // ascent + descent + leading
    float pixelSize;  // size the atlas was rasterized at
    float capHeight;  // ink height of 'H', for vertical centering
};

struct UiState;

UiState *UiInit(Arena *arena, float drawableWidth, float drawableHeight);
void UiHandleResize(UiState *ui, float drawableWidth, float drawableHeight);

// Global UI zoom (Cmd + / Cmd -). 1.0 is native; clamped to a sane range. The
// layout is unchanged — only the on-screen size of every panel, control, and
// glyph scales.
void UiSetUiScale(UiState *ui, float scale);
void UiAdjustUiScale(UiState *ui, float delta);
float UiUiScale(const UiState *ui);

// Magenta outlines on every UI container, for auditing inset symmetry.
void UiSetDebugLayout(UiState *ui, bool enabled);

// Forget every panel's placement. The next UiBuildFrame recreates the slots, so
// each panel returns to the dock and size its UiBeginPanel call site asks for.
void UiResetPanels(UiState *ui);

// The UI renderer bakes the glyph atlas and owns these metrics; the app hands
// the pointer in once at startup. It must outlive `ui`. Text layout falls back
// to a fixed 8px cell until this is set.
void UiSetFontMetrics(UiState *ui, const UiFontMetrics *metrics);

// `menuContext` supplies the menu strip's Show-Menu-Bar preference (via
// menuContext.menuState) and the fullscreen flag its command predicates read;
// the UI never invokes commands, only reports which was clicked.
void UiBuildFrame(UiState *ui, FrameInput input, CommandContext menuContext);

// The viewport marquee rectangle in drawable pixels, set by the app each frame
// from its box-select drag state (two opposite corners; order-independent).
// `active` false hides it.
void UiSetMarquee(UiState *ui, bool active, float x0, float y0, float x1, float y1);

// A command chosen in the in-app menu strip this frame, or Command_None.
// Reading it clears it.
CommandId UiTakeCommand(UiState *ui);

// 3D-editor toolbar + panel bindings. The app hands these to the UI once per
// frame via UiSetEditorState, before UiBuildFrame. Any pointer left null makes
// the controls that need it inert, so the UI still builds before the modules
// are wired. The toolbar reads and writes the tool mode straight through
// `scene` (SceneToolMode / SceneSetToolMode).
struct TimelineState;
struct SceneState;
struct AudioState;

struct UiEditorState {
    bool requestAddMenu; // app raises this for one frame (the 'n' key) to open
                         // the outliner's add-kind dropdown
    TimelineState *timeline;
    SceneState *scene;
    AudioState *audio;
};

void UiSetEditorState(UiState *ui, UiEditorState editor);

// Panels. A panel is a titled, dockable, tear-off container: drag its title
// bar to move it, drop near a screen edge to dock or anywhere else to float.
// `id` is a stable nonzero caller value; `initialDock` / `initialSize` apply
// only the first time a given id is seen. Build the panel's controls between
// UiBeginPanel and UiEndPanel; the calls stay unconditional — while the
// title bar is being dragged the control calls are internally suppressed and
// only a drop outline is drawn.
enum {
    UiDock_Float = 0,
    UiDock_Left,
    UiDock_Right,
    UiDock_Top,
    UiDock_Bottom,
};

typedef int UiPanelId;

void UiBeginPanel(UiState *ui, UiPanelId id, const char *title, int initialDock, float initialSize);
void UiEndPanel(UiState *ui);
bool UiPanelButton(UiState *ui, const char *label);
void UiPanelSlider(UiState *ui, const char *label, float *value);
void UiPanelText(UiState *ui, const char *text);

// Declares the body size (inside the padding) the current panel's contents need.
// The panel is never docked or floated smaller than this. The UiPanel* controls
// declare themselves; bodies laid out by hand call this directly.
void UiPanelContentMin(UiState *ui, float width, float height);

// True when the cursor is over UI chrome (any panel, the menu strip, an open
// dropdown) or a UI drag is in progress. The platform layer withholds camera
// input on frames where UiWantsMouse is true.
bool UiWantsMouse(const UiState *ui);
bool UiWantsKeyboard(const UiState *ui);

// The rectangle the 3D scene renders into: the drawable minus the menu strip
// and every docked panel. Floating panels draw on top and do not shrink it.
float UiContentOriginX(const UiState *ui);
float UiContentOriginY(const UiState *ui);
float UiContentWidth(const UiState *ui);
float UiContentHeight(const UiState *ui);
float UiDrawableWidth(const UiState *ui);
float UiDrawableHeight(const UiState *ui);

const UiVertex *UiVertices(const UiState *ui);
int UiVertexCount(const UiState *ui);
