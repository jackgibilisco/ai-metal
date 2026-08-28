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

// App-owned values the demo controls edit, passed into UiBuildFrame by pointer.
struct UiDemoState {
    float speed;
    float zoom;
};

struct UiState;

UiState *UiInit(Arena *arena, float drawableWidth, float drawableHeight);
void UiHandleResize(UiState *ui, float drawableWidth, float drawableHeight);

// `menuContext` supplies the menu strip's Show-Menu-Bar preference (via
// menuContext.menuState) and the fullscreen flag its command predicates read;
// the UI never invokes commands, only reports which was clicked. `demo` is
// app-owned state the sliders mutate.
void UiBuildFrame(UiState *ui, FrameInput input, CommandContext menuContext, UiDemoState *demo);

// A command chosen in the in-app menu strip this frame, or Command_None.
// Reading it clears it.
CommandId UiTakeCommand(UiState *ui);

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
