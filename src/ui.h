#pragma once

// Immediate-mode UI core. Pure C++: no Metal, no AppKit. Each frame it reads
// one FrameInput, runs the widget calls, and rebuilds a flat triangle list in
// screen-pixel coordinates (top-left origin). A Metal/D3D backend turns that
// list into draw calls.
//
// UiState holds view state only (panel width, open menu, hot/active widget,
// previous input, the vertex buffer). The UI renders app/game state passed in
// and reports intents back out; it does not own domain data.

#include "arena.h"
#include "frame_input.h"
#include "menu.h"

struct UiVertex {
    float x;
    float y;
    float u;
    float v;
    float mode; // 0 = solid color; 1 reserved for textured glyphs
    unsigned char rgba[4];
};

// App-owned values the demo widgets edit, passed into UiBuildFrame by pointer.
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

// True when the cursor is over UI chrome (panel, splitter, menu strip, open
// dropdown) or a UI drag is in progress. The platform layer withholds camera
// input on frames where UiWantsMouse is true.
bool UiWantsMouse(const UiState *ui);
bool UiWantsKeyboard(const UiState *ui);

// The rectangle the 3D scene renders into: the drawable minus the right
// panel + splitter (width) and the top menu strip (origin Y).
float UiContentOriginX(const UiState *ui);
float UiContentOriginY(const UiState *ui);
float UiContentWidth(const UiState *ui);
float UiContentHeight(const UiState *ui);
float UiDrawableWidth(const UiState *ui);
float UiDrawableHeight(const UiState *ui);

const UiVertex *UiVertices(const UiState *ui);
int UiVertexCount(const UiState *ui);
