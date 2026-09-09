#include "ui.h"

#include "audio.h"
#include "scene.h"
#include "theme.h"
#include "timeline.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr float kPadding = 14.0f;
constexpr float kRowGap = 9.0f;
constexpr float kButtonHeight = 30.0f;
constexpr float kSliderHeight = 46.0f;
// Fallback cell size when the proportional font metrics have not been handed
// in yet (see UiSetFontMetrics).
constexpr float kTextScale = 2.0f;
constexpr float kGlyphPixels = 8.0f;
constexpr float kFallbackLineHeight = kGlyphPixels * kTextScale;

constexpr float kMenuBarHeight = 32.0f;
constexpr float kMenuTitlePadX = 12.0f;
constexpr float kMenuRowHeight = 26.0f;
constexpr float kMenuDropdownPadX = 14.0f;
constexpr float kMenuDropdownMinWidth = 180.0f;
constexpr float kMenuShortcutColumn = 72.0f;
constexpr float kMenuCheckColumn = 26.0f; // left gutter for the checkmark dot

constexpr int kMaxPanels = 4;
constexpr float kTitleBarHeight = 24.0f;
constexpr float kPanelBorderPx = 1.0f;
constexpr float kResizeGripPx = 6.0f;
constexpr float kDockSnapMargin = 56.0f;   // cursor within this of an edge -> dock preview
constexpr float kPanelMinDockW = 200.0f;   // Left/Right docks (dockSize is a width)
constexpr float kPanelMinDockH = 150.0f;   // Top/Bottom docks (dockSize is a height)
constexpr float kDockMaxFraction = 0.6f;
constexpr float kFloatMinW = 200.0f;
constexpr float kFloatMinH = 150.0f;

constexpr int kMaxVertices = 65536;

// Every color value lives in theme.h. These names are the local spelling the
// draw code below already uses; each is exactly one palette entry.
using Color = theme::Color;

constexpr Color kPanelBg = theme::PanelBg;
constexpr Color kTitleBarCol = theme::TitleBar;
constexpr Color kTitleBarHot = theme::TitleBarHot;
constexpr Color kPanelBorderCol = theme::PanelBorder;
constexpr Color kResizeGripCol = theme::ResizeGrip;
constexpr Color kResizeGripHot = theme::ResizeGripHot;
constexpr Color kDragOutlineCol = theme::DragOutline;
constexpr Color kButtonCol = theme::Button;
constexpr Color kButtonHot = theme::ButtonHot;
constexpr Color kButtonActive = theme::ButtonActive;
constexpr Color kTrackCol = theme::SliderTrack;
constexpr Color kHandleCol = theme::SliderHandle;
constexpr Color kHandleHot = theme::SliderHandleHot;
constexpr Color kTextCol = theme::Text;
constexpr Color kTextDisabled = theme::TextDisabled;
constexpr Color kTextShortcut = theme::TextShortcut;
constexpr Color kMenuBarBg = theme::MenuBarBg;
constexpr Color kMenuTitleHot = theme::MenuTitleHot;
constexpr Color kMenuTitleOpen = theme::MenuTitleOpen;
constexpr Color kMenuDropdownBg = theme::MenuDropdownBg;
constexpr Color kMenuItemHot = theme::MenuItemHot;
constexpr Color kMenuSeparatorCol = theme::MenuSeparator;

struct Rect {
    float x, y, w, h;
};

bool PointInRect(float px, float py, Rect r) {
    return px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h;
}

float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

struct PanelState {
    int id; // 0 = empty slot
    int dock;
    float dockSize;  // width for Left/Right, height for Top/Bottom
    Rect floatRect;
};

} // namespace

struct UiState {
    float drawableWidth;
    float drawableHeight;

    MenuBar menuBar;
    CommandContext menuContext; // set each frame; menu rendering queries it
    int openMenu;               // -1 when closed
    CommandId pendingCommand;
    bool menuBarVisible;
    float menuBarHeight; // 0 when the strip is hidden
    bool menuHasPointer; // the strip/dropdown owns the cursor this frame

    PanelState panels[kMaxPanels];
    Rect panelRects[kMaxPanels]; // resolved this frame; parallel to panels[]
    Rect contentRect;            // the 3D viewport: drawable minus strip and docked panels

    UiEditorState editor;   // set each frame by the app; zero until then
    Rect toolbarRect;       // the viewport toolbar strip, resolved this frame

    int draggedPanel;  // id, 0 = none (retained across frames while held)
    float dragGrabX;   // cursor offset within the title bar at grab
    float dragGrabY;
    float dragW; // floating size the panel takes if dropped free
    float dragH;
    int dragPreviewDock;
    Rect dragOutline;
    int resizePanel; // id of the panel whose dock edge is being dragged, 0 = none

    int currentPanel; // id between UiBeginPanel/UiEndPanel, 0 = none
    Rect currentBody; // control layout area inside the current panel
    float panelCursorY;
    int controlIndex;
    bool suppressBody; // current panel is being torn off: skip its controls

    TimelineClipId timelineDragClip; // clip being moved or trimmed, 0 = none
    int timelineDragMode;
    float timelineDragGrabX;      // mouseX when the drag started
    double timelineDragStartTime; // the clip's values at grab, to offset against
    double timelineDragDuration;
    double timelineDragTrimIn;
    double timelineDragTrimOut;

    int hotId;
    int activeId;
    bool prevMouseDown;

    float mouseX;
    float mouseY;
    bool mouseDown;
    bool mousePressed;
    bool mouseReleased;

    // Scene-outliner inline rename + add-kind dropdown.
    int focusedInputId; // 0 = no text field has the keyboard
    char editBuffer[64];
    int editLen;
    int editCaret;
    bool addMenuOpen;
    unsigned int frameIndex;    // ++ per UiBuildFrame; drives caret blink + double-click timing
    int lastClickControlId;
    unsigned int lastClickFrame;

    bool marqueeActive;
    Rect marqueeRect;

    // Timeline view: horizontal pan (seconds at the ruler's left edge), zoom
    // (px per second), vertical lane pan (pixels).
    double timelineScrollSeconds;
    float timelinePixelsPerSecond;
    float timelineLaneScroll;

    // Global UI zoom. The whole immediate-mode layout runs in logical units;
    // logical = realPixels / uiScale. Vertices scale up on push, incoming
    // pointer coords scale down, size accessors report real pixels.
    float uiScale;
    float realDrawableWidth;
    float realDrawableHeight;

    const KeyEvent *frameKeys; // this frame's key events; valid only during UiBuildFrame
    int frameKeyCount;

    UiVertex vertices[kMaxVertices];
    int vertexCount;
};

namespace {

void PushVertexUV(UiState *ui, float x, float y, float u, float v, float mode, Color c) {
    assert(ui->vertexCount < kMaxVertices);
    UiVertex &vert = ui->vertices[ui->vertexCount++];
    vert.x = x * ui->uiScale;
    vert.y = y * ui->uiScale;
    vert.u = u;
    vert.v = v;
    vert.mode = mode;
    vert.rgba[0] = c.r;
    vert.rgba[1] = c.g;
    vert.rgba[2] = c.b;
    vert.rgba[3] = c.a;
}

void PushVertex(UiState *ui, float x, float y, Color c) {
    PushVertexUV(ui, x, y, 0.0f, 0.0f, 0.0f, c);
}

// Proportional font metrics, baked and owned by the UI renderer, handed in once
// via UiSetFontMetrics. Immutable for the process, so a file-scope pointer
// rather than threading it through every text call.
const UiFontMetrics *g_font = nullptr;

// Nominal text-line box height. Callers reserve this and centre text in it;
// PushText places the baseline so the CAP height (not the font's loose
// ascent/descent) sits centred in the same box.
float TextLineHeight() { return g_font != nullptr ? g_font->pixelSize : kFallbackLineHeight; }

// Cap height from a reference uppercase glyph, for visual vertical centring.
float TextCapHeight() { return g_font != nullptr ? g_font->capHeight : kGlyphPixels; }

const UiGlyphMetric *GlyphFor(int codepoint) {
    if (codepoint < 32 || codepoint >= 128) {
        return &g_font->glyphs[0]; // treat unknowns as a space
    }
    return &g_font->glyphs[codepoint - 32];
}

void PushQuad(UiState *ui, float x0, float y0, float x1, float y1, float x2, float y2, float x3,
              float y3, Color c) {
    PushVertex(ui, x0, y0, c);
    PushVertex(ui, x1, y1, c);
    PushVertex(ui, x2, y2, c);
    PushVertex(ui, x0, y0, c);
    PushVertex(ui, x2, y2, c);
    PushVertex(ui, x3, y3, c);
}

void PushRect(UiState *ui, Rect r, Color c) {
    PushQuad(ui, r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h, c);
}

void PushBorder(UiState *ui, Rect r, float t, Color c) {
    PushRect(ui, {r.x, r.y, r.w, t}, c);
    PushRect(ui, {r.x, r.y + r.h - t, r.w, t}, c);
    PushRect(ui, {r.x, r.y, t, r.h}, c);
    PushRect(ui, {r.x + r.w - t, r.y, t, r.h}, c);
}

void PushGlyphQuad(UiState *ui, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                   float v1, Color c) {
    PushVertexUV(ui, x0, y0, u0, v0, 1.0f, c);
    PushVertexUV(ui, x1, y0, u1, v0, 1.0f, c);
    PushVertexUV(ui, x1, y1, u1, v1, 1.0f, c);
    PushVertexUV(ui, x0, y0, u0, v0, 1.0f, c);
    PushVertexUV(ui, x1, y1, u1, v1, 1.0f, c);
    PushVertexUV(ui, x0, y1, u0, v1, 1.0f, c);
}

// `y` is the top of a TextLineHeight()-tall line; glyphs sit on the baseline
// TextLineHeight()*ascent-ratio down from it.
void PushText(UiState *ui, float x, float y, const char *text, Color c) {
    if (g_font == nullptr) {
        return;
    }
    float penX = x;
    float baseline = y + (TextLineHeight() + TextCapHeight()) * 0.5f;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        const UiGlyphMetric *g = GlyphFor(*p);
        if (g->width > 0.0f) {
            float gx = penX + g->offsetX;
            float gy = baseline - g->offsetY;
            PushGlyphQuad(ui, gx, gy, gx + g->width, gy + g->height, g->u0, g->v0, g->u1, g->v1, c);
        }
        penX += g->advance;
    }
}

float TextWidth(const char *text) {
    if (g_font == nullptr) {
        return (float)strlen(text) * kFallbackLineHeight;
    }
    float w = 0.0f;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        w += GlyphFor(*p)->advance;
    }
    return w;
}

float TextWidthN(const char *text, int n) {
    if (g_font == nullptr) {
        return (float)n * kFallbackLineHeight;
    }
    float w = 0.0f;
    for (int i = 0; i < n && text[i] != '\0'; ++i) {
        w += GlyphFor((unsigned char)text[i])->advance;
    }
    return w;
}

bool Button(UiState *ui, int id, Rect r, const char *label) {
    bool inside = PointInRect(ui->mouseX, ui->mouseY, r);
    if (inside) {
        ui->hotId = id;
    }
    if (inside && ui->mousePressed) {
        ui->activeId = id;
    }
    bool clicked = ui->activeId == id && ui->mouseReleased && inside;

    Color background = kButtonCol;
    if (ui->activeId == id && ui->mouseDown && inside) {
        background = kButtonActive;
    } else if (ui->hotId == id) {
        background = kButtonHot;
    }
    PushRect(ui, r, background);

    float textWidth = TextWidth(label);
    float textX = r.x + (r.w - textWidth) * 0.5f;
    float textY = r.y + (r.h - TextLineHeight()) * 0.5f;
    PushText(ui, textX, textY, label, kTextCol);
    return clicked;
}

void Slider(UiState *ui, int id, Rect r, const char *label, float *value) {
    float trackY = r.y + 26.0f;
    Rect track = {r.x, trackY, r.w, 6.0f};
    Rect hitArea = {r.x, r.y + 16.0f, r.w, r.h - 16.0f};

    bool inside = PointInRect(ui->mouseX, ui->mouseY, hitArea);
    if (inside) {
        ui->hotId = id;
    }
    if (inside && ui->mousePressed) {
        ui->activeId = id;
    }
    if (ui->activeId == id && ui->mouseDown) {
        float t = (ui->mouseX - r.x) / r.w;
        *value = Clamp(t, 0.0f, 1.0f);
    }

    PushText(ui, r.x, r.y, label, kTextCol);

    char readout[32];
    snprintf(readout, sizeof(readout), "%.2f", (double)*value);
    PushText(ui, r.x + r.w - TextWidth(readout), r.y, readout, kTextCol);

    PushRect(ui, track, kTrackCol);

    float handleX = r.x + (*value) * (r.w - 12.0f);
    Color handleColor = (ui->hotId == id || ui->activeId == id) ? kHandleHot : kHandleCol;
    PushRect(ui, {handleX, trackY - 8.0f, 12.0f, 22.0f}, handleColor);
}

// ---- menu strip ---------------------------------------------------------

float MenuTitleX(const UiState *ui, int menuIndex) {
    float x = kMenuTitlePadX;
    for (int i = 0; i < menuIndex; ++i) {
        x += TextWidth(ui->menuBar.menus[i].title) + kMenuTitlePadX * 2.0f;
    }
    return x;
}

Rect MenuTitleRect(const UiState *ui, int menuIndex) {
    float w = TextWidth(ui->menuBar.menus[menuIndex].title) + kMenuTitlePadX * 2.0f;
    return {MenuTitleX(ui, menuIndex), 0.0f, w, kMenuBarHeight};
}

void ShortcutLabel(Shortcut shortcut, char *buffer, size_t size) {
    if (shortcut.key == 0) {
        buffer[0] = '\0';
        return;
    }
    char key = (char)(shortcut.key >= 'a' && shortcut.key <= 'z' ? shortcut.key - 32 : shortcut.key);
    snprintf(buffer, size, "%s%s%s%s%c", (shortcut.mods & ShortcutMod_Ctrl) ? "Ctrl+" : "",
             (shortcut.mods & ShortcutMod_Alt) ? "Alt+" : "",
             (shortcut.mods & ShortcutMod_Shift) ? "Shift+" : "",
             (shortcut.mods & ShortcutMod_Cmd) ? "Cmd+" : "", key);
}

Rect MenuDropdownRect(const UiState *ui, int menuIndex) {
    const Menu &menu = ui->menuBar.menus[menuIndex];
    float width = kMenuDropdownMinWidth;
    for (int i = 0; i < menu.entryCount; ++i) {
        const Command *command = CommandById(menu.entries[i]);
        if (command == nullptr) {
            continue;
        }
        float w = kMenuCheckColumn + TextWidth(command->label) + kMenuDropdownPadX * 2.0f;
        char combo[16];
        ShortcutLabel(command->shortcut, combo, sizeof(combo));
        if (combo[0] != '\0') {
            w += kMenuShortcutColumn;
        }
        if (w > width) {
            width = w;
        }
    }
    return {MenuTitleX(ui, menuIndex), kMenuBarHeight, width,
            (float)menu.entryCount * kMenuRowHeight};
}

bool MenuEntryEnabled(const UiState *ui, const Command *command) {
    return command->isEnabled == nullptr || command->isEnabled(ui->menuContext);
}

void MenuUpdate(UiState *ui) {
    ui->menuHasPointer = false;
    if (!ui->menuBarVisible) {
        ui->openMenu = -1;
        return;
    }

    bool pointerInBar = ui->mouseY >= 0.0f && ui->mouseY < kMenuBarHeight &&
                        ui->mouseX >= 0.0f && ui->mouseX < ui->drawableWidth;

    int hoveredTitle = -1;
    for (int i = 0; i < ui->menuBar.menuCount; ++i) {
        if (PointInRect(ui->mouseX, ui->mouseY, MenuTitleRect(ui, i))) {
            hoveredTitle = i;
        }
    }

    if (ui->mousePressed && hoveredTitle >= 0) {
        ui->openMenu = (ui->openMenu == hoveredTitle) ? -1 : hoveredTitle;
    } else if (ui->openMenu >= 0) {
        Rect dropdown = MenuDropdownRect(ui, ui->openMenu);
        if (PointInRect(ui->mouseX, ui->mouseY, dropdown)) {
            ui->menuHasPointer = true;
            int row = (int)((ui->mouseY - dropdown.y) / kMenuRowHeight);
            const Menu &menu = ui->menuBar.menus[ui->openMenu];
            if (ui->mousePressed && row >= 0 && row < menu.entryCount) {
                const Command *command = CommandById(menu.entries[row]);
                if (command != nullptr && MenuEntryEnabled(ui, command)) {
                    ui->pendingCommand = command->id;
                    ui->openMenu = -1;
                }
            }
        } else if (ui->mousePressed) {
            ui->openMenu = -1;
        }
    }

    if (pointerInBar) {
        ui->menuHasPointer = true;
    }
}

void MenuDraw(UiState *ui) {
    if (!ui->menuBarVisible) {
        return;
    }

    PushRect(ui, {0.0f, 0.0f, ui->drawableWidth, kMenuBarHeight}, kMenuBarBg);

    float titleTextY = (kMenuBarHeight - TextLineHeight()) * 0.5f;
    for (int i = 0; i < ui->menuBar.menuCount; ++i) {
        Rect titleRect = MenuTitleRect(ui, i);
        bool hovered = PointInRect(ui->mouseX, ui->mouseY, titleRect);
        if (ui->openMenu == i) {
            PushRect(ui, titleRect, kMenuTitleOpen);
        } else if (hovered) {
            PushRect(ui, titleRect, kMenuTitleHot);
        }
        PushText(ui, titleRect.x + kMenuTitlePadX, titleTextY, ui->menuBar.menus[i].title, kTextCol);
    }

    if (ui->openMenu < 0) {
        return;
    }

    const Menu &menu = ui->menuBar.menus[ui->openMenu];
    Rect dropdown = MenuDropdownRect(ui, ui->openMenu);
    PushRect(ui, dropdown, kMenuDropdownBg);

    for (int i = 0; i < menu.entryCount; ++i) {
        Rect row = {dropdown.x, dropdown.y + (float)i * kMenuRowHeight, dropdown.w, kMenuRowHeight};
        float textY = row.y + (kMenuRowHeight - TextLineHeight()) * 0.5f;

        const Command *command = CommandById(menu.entries[i]);
        if (command == nullptr) {
            float lineY = row.y + kMenuRowHeight * 0.5f;
            PushRect(ui, {row.x + kMenuDropdownPadX, lineY, row.w - kMenuDropdownPadX * 2.0f, 1.0f},
                     kMenuSeparatorCol);
            continue;
        }

        bool enabled = MenuEntryEnabled(ui, command);
        if (enabled && PointInRect(ui->mouseX, ui->mouseY, row)) {
            PushRect(ui, row, kMenuItemHot);
        }
        if (command->isChecked != nullptr && command->isChecked(ui->menuContext)) {
            PushRect(ui, {row.x + 6.0f, textY + 2.0f, 6.0f, 6.0f}, kTextCol);
        }
        PushText(ui, row.x + kMenuCheckColumn, textY, command->label,
                 enabled ? kTextCol : kTextDisabled);

        char combo[16];
        ShortcutLabel(command->shortcut, combo, sizeof(combo));
        if (combo[0] != '\0') {
            PushText(ui, row.x + row.w - kMenuDropdownPadX - TextWidth(combo), textY, combo,
                     kTextShortcut);
        }
    }
}

// ---- panels -----------------------------------------------------------

int PanelSlot(const UiState *ui, int id) {
    for (int i = 0; i < kMaxPanels; ++i) {
        if (ui->panels[i].id == id) {
            return i;
        }
    }
    return -1;
}

int PanelSlotOrCreate(UiState *ui, int id, int initialDock, float initialSize) {
    int slot = PanelSlot(ui, id);
    if (slot >= 0) {
        return slot;
    }
    for (int i = 0; i < kMaxPanels; ++i) {
        if (ui->panels[i].id != 0) {
            continue;
        }
        PanelState &p = ui->panels[i];
        p.id = id;
        p.dock = initialDock;
        p.dockSize = initialSize;
        p.floatRect = {60.0f + (float)i * 26.0f, kMenuBarHeight + 46.0f + (float)i * 26.0f, 264.0f,
                       208.0f};
        return i;
    }
    return 0;
}

Rect ClampFloatRect(const UiState *ui, Rect r) {
    float top = ui->menuBarHeight;
    r.w = Clamp(r.w, kFloatMinW, ui->drawableWidth);
    r.h = Clamp(r.h, kFloatMinH, ui->drawableHeight - top);
    r.x = Clamp(r.x, 0.0f, ui->drawableWidth - r.w);
    r.y = Clamp(r.y, top, ui->drawableHeight - r.h);
    return r;
}

float DockLimit(const UiState *ui, int dock) {
    if (dock == UiDock_Left || dock == UiDock_Right) {
        return ui->drawableWidth * kDockMaxFraction;
    }
    return (ui->drawableHeight - ui->menuBarHeight) * kDockMaxFraction;
}

float DockMinSize(int dock) {
    return (dock == UiDock_Left || dock == UiDock_Right) ? kPanelMinDockW : kPanelMinDockH;
}

Rect DockRect(const UiState *ui, int dock, float size) {
    float top = ui->menuBarHeight;
    float fullH = ui->drawableHeight - top;
    if (dock == UiDock_Left) {
        return {0.0f, top, size, fullH};
    }
    if (dock == UiDock_Right) {
        return {ui->drawableWidth - size, top, size, fullH};
    }
    if (dock == UiDock_Top) {
        return {0.0f, top, ui->drawableWidth, size};
    }
    return {0.0f, ui->drawableHeight - size, ui->drawableWidth, size};
}

void ResolvePanelLayout(UiState *ui) {
    Rect free = {0.0f, ui->menuBarHeight, ui->drawableWidth, ui->drawableHeight - ui->menuBarHeight};

    for (int i = 0; i < kMaxPanels; ++i) {
        ui->panelRects[i] = {0.0f, 0.0f, 0.0f, 0.0f};
    }

    // Docked panels resolve in a fixed dock priority, not panel order: Top and
    // Bottom claim the full width first, then Left and Right split the band
    // between them. So a bottom-docked timeline spans the whole window and the
    // side panels stack above it. Sizes are floored so every panel edge and
    // the viewport rect land on the same integer pixel grid (no 1px seam).
    const int dockOrder[] = {UiDock_Top, UiDock_Bottom, UiDock_Left, UiDock_Right};
    for (int pass = 0; pass < 4; ++pass) {
        int dock = dockOrder[pass];
        for (int i = 0; i < kMaxPanels; ++i) {
            PanelState &p = ui->panels[i];
            if (p.id == 0 || p.id == ui->draggedPanel || p.dock != dock) {
                continue;
            }
            float size = floorf(Clamp(p.dockSize, DockMinSize(dock), DockLimit(ui, dock)));
            if (dock == UiDock_Left) {
                ui->panelRects[i] = {free.x, free.y, size, free.h};
                free.x += size;
                free.w -= size;
            } else if (dock == UiDock_Right) {
                ui->panelRects[i] = {free.x + free.w - size, free.y, size, free.h};
                free.w -= size;
            } else if (dock == UiDock_Top) {
                ui->panelRects[i] = {free.x, free.y, free.w, size};
                free.y += size;
                free.h -= size;
            } else {
                ui->panelRects[i] = {free.x, free.y + free.h - size, free.w, size};
                free.h -= size;
            }
        }
    }

    for (int i = 0; i < kMaxPanels; ++i) {
        PanelState &p = ui->panels[i];
        if (p.id != 0 && p.id != ui->draggedPanel && p.dock == UiDock_Float) {
            ui->panelRects[i] = ClampFloatRect(ui, p.floatRect);
        }
    }

    free.x = floorf(free.x);
    free.y = floorf(free.y);
    free.w = floorf(free.w);
    free.h = floorf(free.h);
    if (free.w < 1.0f) {
        free.w = 1.0f;
    }
    if (free.h < 1.0f) {
        free.h = 1.0f;
    }
    ui->contentRect = free;
}

Rect ResizeGripRect(const UiState *ui, int slot) {
    const PanelState &p = ui->panels[slot];
    if (p.id == 0 || p.id == ui->draggedPanel || p.dock == UiDock_Float) {
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    Rect r = ui->panelRects[slot];
    if (r.w <= 0.0f) {
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    float g = kResizeGripPx;
    if (p.dock == UiDock_Left) {
        return {r.x + r.w - g, r.y, g, r.h};
    }
    if (p.dock == UiDock_Right) {
        return {r.x, r.y, g, r.h};
    }
    if (p.dock == UiDock_Top) {
        return {r.x, r.y + r.h - g, r.w, g};
    }
    return {r.x, r.y, r.w, g};
}

int EdgeDockPreview(const UiState *ui) {
    float top = ui->menuBarHeight;
    if (ui->mouseX < kDockSnapMargin) {
        return UiDock_Left;
    }
    if (ui->mouseX > ui->drawableWidth - kDockSnapMargin) {
        return UiDock_Right;
    }
    if (ui->mouseY < top + kDockSnapMargin) {
        return UiDock_Top;
    }
    if (ui->mouseY > ui->drawableHeight - kDockSnapMargin) {
        return UiDock_Bottom;
    }
    return UiDock_Float;
}

void UpdatePanelInteraction(UiState *ui) {
    if (ui->draggedPanel != 0) {
        int slot = PanelSlot(ui, ui->draggedPanel);
        int preview = EdgeDockPreview(ui);
        Rect outline;
        if (preview == UiDock_Float) {
            outline = {ui->mouseX - ui->dragGrabX, ui->mouseY - ui->dragGrabY, ui->dragW, ui->dragH};
        } else {
            float want = (preview == UiDock_Left || preview == UiDock_Right) ? ui->dragW : ui->dragH;
            outline = DockRect(ui, preview, Clamp(want, DockMinSize(preview), DockLimit(ui, preview)));
        }
        ui->dragOutline = outline;
        ui->dragPreviewDock = preview;

        if (!ui->mouseDown && slot >= 0) {
            PanelState &p = ui->panels[slot];
            if (preview == UiDock_Float) {
                p.dock = UiDock_Float;
                p.floatRect = ClampFloatRect(ui, outline);
            } else {
                p.dock = preview;
                p.dockSize = (preview == UiDock_Left || preview == UiDock_Right) ? outline.w : outline.h;
            }
            ui->draggedPanel = 0;
        }
        return;
    }

    if (ui->resizePanel != 0) {
        int slot = PanelSlot(ui, ui->resizePanel);
        if (slot >= 0) {
            PanelState &p = ui->panels[slot];
            Rect r = ui->panelRects[slot];
            float size = p.dockSize;
            if (p.dock == UiDock_Left) {
                size = ui->mouseX - r.x;
            } else if (p.dock == UiDock_Right) {
                size = r.x + r.w - ui->mouseX;
            } else if (p.dock == UiDock_Top) {
                size = ui->mouseY - r.y;
            } else if (p.dock == UiDock_Bottom) {
                size = r.y + r.h - ui->mouseY;
            }
            p.dockSize = Clamp(size, DockMinSize(p.dock), DockLimit(ui, p.dock));
        }
        if (!ui->mouseDown) {
            ui->resizePanel = 0;
        }
        return;
    }

    if (!ui->mousePressed) {
        return;
    }

    for (int i = 0; i < kMaxPanels; ++i) {
        if (ui->panels[i].id == 0) {
            continue;
        }
        Rect grip = ResizeGripRect(ui, i);
        if (grip.w > 0.0f && PointInRect(ui->mouseX, ui->mouseY, grip)) {
            ui->resizePanel = ui->panels[i].id;
            ui->mousePressed = false;
            return;
        }
    }

    for (int i = 0; i < kMaxPanels; ++i) {
        if (ui->panels[i].id == 0) {
            continue;
        }
        Rect r = ui->panelRects[i];
        if (r.w <= 0.0f) {
            continue;
        }
        Rect titleBar = {r.x, r.y, r.w, kTitleBarHeight};
        if (PointInRect(ui->mouseX, ui->mouseY, titleBar)) {
            ui->draggedPanel = ui->panels[i].id;
            ui->dragGrabX = ui->mouseX - r.x;
            ui->dragGrabY = ui->mouseY - r.y;
            ui->dragW = Clamp(ui->panels[i].floatRect.w, kFloatMinW, 420.0f);
            ui->dragH = Clamp(ui->panels[i].floatRect.h, kFloatMinH, 360.0f);
            if (ui->dragGrabX > ui->dragW) {
                ui->dragGrabX = ui->dragW * 0.5f;
            }
            ui->mousePressed = false;
            return;
        }
    }
}

// ---- 3D-editor toolbar ----------------------------------------------

struct ToolButtonDef {
    const char *label;
    const char *shortcut; // shown in the hover tooltip
    ToolMode mode;        // tool mode this button selects
};

const ToolButtonDef kToolButtons[] = {
    {"Move", "1", ToolMode_Translate},
    {"Rotate", "2", ToolMode_Rotate},
    {"Scale", "3", ToolMode_Scale},
};
constexpr int kToolButtonCount = (int)(sizeof(kToolButtons) / sizeof(kToolButtons[0]));

constexpr float kToolbarMargin = 10.0f;
constexpr float kToolbarInsetX = 12.0f;
constexpr float kToolButtonHeight = 26.0f;
constexpr float kToolButtonGap = 4.0f;
constexpr float kToolbarInsetY = 6.0f;

bool ToolButtonOn(const ToolButtonDef &def, const UiEditorState *e) {
    return e->scene != nullptr && SceneToolMode(e->scene) == def.mode;
}

bool ToolButtonWidget(UiState *ui, int id, Rect r, const char *label, bool on) {
    bool inside = PointInRect(ui->mouseX, ui->mouseY, r);
    if (inside) {
        ui->hotId = id;
    }
    if (inside && ui->mousePressed) {
        ui->activeId = id;
    }
    bool clicked = ui->activeId == id && ui->mouseReleased && inside;

    Color background = theme::ToolButton;
    if (on || (ui->activeId == id && ui->mouseDown && inside)) {
        background = theme::ToolButtonActive;
    } else if (ui->hotId == id) {
        background = theme::ToolButtonHot;
    }
    PushRect(ui, r, background);

    float textX = r.x + (r.w - TextWidth(label)) * 0.5f;
    float textY = r.y + (r.h - TextLineHeight()) * 0.5f;
    PushText(ui, textX, textY, label, kTextCol);
    return clicked;
}

constexpr float kTimelineMinPixelsPerSecond = 6.0f;
constexpr float kTimelineMaxPixelsPerSecond = 400.0f;
constexpr float kTimelineTransportHeight = 26.0f;
constexpr float kTimelineRulerHeight = 28.0f;

// Track header: two stacked text rows (name over volume + mute/solo). A row is
// TextLineHeight() tall (20 px); the lane height is derived so the rows and
// their padding always fit.
constexpr float kTimelineHeaderPad = 6.0f;
constexpr float kTimelineHeaderRowH = 20.0f;
constexpr float kTimelineHeaderRowGap = 6.0f;
constexpr float kTimelineLaneHeight =
    kTimelineHeaderPad * 2.0f + kTimelineHeaderRowH * 2.0f + kTimelineHeaderRowGap;
constexpr float kTimelineHeaderWidth = 184.0f;
constexpr float kTimelineTrimHandleWidth = 6.0f;
constexpr float kTimelinePlayheadGrabPx = 5.0f;
constexpr int kTimelineRulerLabelSeconds = 5;
constexpr double kTimelineMinClipSeconds = 0.05;

constexpr int kTimelinePlayId = 800010;
constexpr int kTimelineStopId = 800011;
constexpr int kTimelinePlayheadId = 800012;
constexpr int kTimelineClipIdBase = 810000;
constexpr int kTimelineTrackCtlBase = 820000; // 4 ids per lane: name, volume, mute, solo

// Inline text edit (defined with the scene outliner below; the timeline track
// headers reuse it for rename).
enum { UiEdit_None = 0, UiEdit_Commit, UiEdit_Cancel };
void BeginTextEdit(UiState *ui, int id, const char *initial);
int TextInputWidget(UiState *ui, Rect r, char *out, int outCap);

enum {
    TimelineDrag_None = 0,
    TimelineDrag_Move,
    TimelineDrag_TrimLeft,
    TimelineDrag_TrimRight,
};

// The lanes visible this frame, resolved from the panel body. `rulerX` is the
// x of transport time 0; everything right of it is time, everything left is the
// per-track header column.
struct TimelineLayout {
    Rect ruler;
    float rulerX;
    float rulerRight;
    float lanesY;
    float bodyBottom;
    float pixelsPerSecond;  // zoom
    double scrollSeconds;   // time at the ruler's left edge
    int laneCount;
    TrackId laneTracks[kMaxTimelineTracks];
    TimelineTrack laneInfo[kMaxTimelineTracks];
};

float TimelineTimeToX(const TimelineLayout &layout, double seconds) {
    return layout.rulerX + (float)((seconds - layout.scrollSeconds) * layout.pixelsPerSecond);
}

double TimelineXToTime(const TimelineLayout &layout, float x) {
    double seconds = layout.scrollSeconds + (double)(x - layout.rulerX) / layout.pixelsPerSecond;
    return seconds < 0.0 ? 0.0 : seconds;
}

int TimelineLaneAtY(const TimelineLayout &layout, float y) {
    if (y < layout.lanesY) {
        return -1;
    }
    int lane = (int)((y - layout.lanesY) / kTimelineLaneHeight);
    return lane < layout.laneCount ? lane : -1;
}

int TimelineLaneOfTrack(const TimelineLayout &layout, TrackId track) {
    for (int i = 0; i < layout.laneCount; ++i) {
        if (layout.laneTracks[i] == track) {
            return i;
        }
    }
    return -1;
}

void FormatTransportTime(double seconds, char *out, size_t size) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    int centiseconds = (int)(seconds * 100.0);
    snprintf(out, size, "%02d:%02d.%02d", centiseconds / 6000, (centiseconds / 100) % 60,
             centiseconds % 100);
}

// The UI vertex list has no scissor, so clip time-domain rects to the ruler span
// by hand or they spill into the track headers.
void PushClippedRect(UiState *ui, Rect r, float clipX0, float clipX1, Color c) {
    float x0 = r.x < clipX0 ? clipX0 : r.x;
    float x1 = (r.x + r.w) > clipX1 ? clipX1 : (r.x + r.w);
    if (x1 <= x0) {
        return;
    }
    PushRect(ui, {x0, r.y, x1 - x0, r.h}, c);
}

EntityId SelectedAudioSource(const SceneState *scene) {
    EntityId active = SceneSelection(scene).activeEntity;
    const Entity *entity = SceneFindEntity(scene, active);
    if (entity != nullptr && entity->kind == EntityKind_AudioSource) {
        return active;
    }
    return kInvalidEntityId;
}

void BuildTimelineTransport(UiState *ui, TimelineState *timeline, Rect row) {
    Rect playRect = {row.x, row.y, 90.0f, row.h};
    if (Button(ui, kTimelinePlayId, playRect, TimelineIsPlaying(timeline) ? "Pause" : "Play")) {
        TimelineTogglePlay(timeline);
    }

    Rect stopRect = {row.x + 96.0f, row.y, 70.0f, row.h};
    if (Button(ui, kTimelineStopId, stopRect, "Stop")) {
        TimelineStopToZero(timeline);
    }

    char readout[16];
    FormatTransportTime(TimelineTime(timeline), readout, sizeof(readout));
    PushText(ui, row.x + 178.0f, row.y + (row.h - TextLineHeight()) * 0.5f, readout,
             kTextCol);
}

void BuildTimelineRuler(UiState *ui, TimelineState *timeline, const TimelineLayout &layout) {
    PushRect(ui, layout.ruler, theme::TimelineRuler);

    int firstSecond = (int)layout.scrollSeconds;
    if (firstSecond < 0) {
        firstSecond = 0;
    }
    double endTime = TimelineXToTime(layout, layout.rulerRight);
    for (int second = firstSecond; (double)second <= endTime; ++second) {
        float x = TimelineTimeToX(layout, second);
        bool labelled = (second % kTimelineRulerLabelSeconds) == 0;
        float tickHeight = labelled ? 10.0f : 5.0f;
        PushRect(ui, {x, layout.ruler.y + layout.ruler.h - tickHeight, 1.0f, tickHeight},
                 theme::TimelineRulerTick);
        if (!labelled) {
            continue;
        }
        char label[16];
        snprintf(label, sizeof(label), "%d:%02d", second / 60, second % 60);
        if (x + TextWidth(label) < layout.rulerRight) {
            PushText(ui, x + 3.0f, layout.ruler.y + 2.0f, label, kTextShortcut);
        }
    }

    float playheadX = TimelineTimeToX(layout, TimelineTime(timeline));
    Rect grab = {playheadX - kTimelinePlayheadGrabPx, layout.ruler.y,
                 kTimelinePlayheadGrabPx * 2.0f, layout.ruler.h};
    if (ui->activeId == 0 && ui->mousePressed) {
        if (PointInRect(ui->mouseX, ui->mouseY, grab)) {
            ui->activeId = kTimelinePlayheadId;
        } else if (PointInRect(ui->mouseX, ui->mouseY, layout.ruler)) {
            TimelineSeek(timeline, TimelineXToTime(layout, ui->mouseX));
        }
    }

    if (ui->activeId == kTimelinePlayheadId) {
        TimelineScrub(timeline, TimelineXToTime(layout, ui->mouseX));
        if (ui->mouseReleased) {
            TimelineScrubEnd(timeline);
        }
    }
}

// Compact slider for a 14 px track-header row; writes *value in [0, 1].
void TimelineHeaderSlider(UiState *ui, int id, Rect r, float *value) {
    bool inside = PointInRect(ui->mouseX, ui->mouseY, r);
    if (inside) {
        ui->hotId = id;
    }
    if (inside && ui->mousePressed) {
        ui->activeId = id;
    }
    if (ui->activeId == id && ui->mouseDown) {
        *value = Clamp((ui->mouseX - r.x) / r.w, 0.0f, 1.0f);
    }
    PushRect(ui, {r.x, r.y + r.h * 0.5f - 2.0f, r.w, 4.0f}, kTrackCol);
    float handleX = r.x + (*value) * (r.w - 8.0f);
    bool hot = ui->hotId == id || ui->activeId == id;
    PushRect(ui, {handleX, r.y, 8.0f, r.h}, hot ? kHandleHot : kHandleCol);
}

// Compact latching toggle; returns true on the click that flipped it.
bool TimelineHeaderToggle(UiState *ui, int id, Rect r, const char *label, bool on) {
    bool inside = PointInRect(ui->mouseX, ui->mouseY, r);
    if (inside) {
        ui->hotId = id;
    }
    if (inside && ui->mousePressed) {
        ui->activeId = id;
    }
    bool clicked = ui->activeId == id && ui->mouseReleased && inside;
    Color bg = on ? kButtonActive : (ui->hotId == id ? kButtonHot : kButtonCol);
    PushRect(ui, r, bg);
    PushText(ui, r.x + (r.w - TextWidth(label)) * 0.5f, r.y + (r.h - TextLineHeight()) * 0.5f, label,
             on ? kTextCol : kTextDisabled);
    return clicked;
}

// Each lane: striped time-domain background plus a header rectangle carrying the
// source name (double-click to rename), a volume slider, and mute / solo. The
// slider and buttons write straight to the source's AudioSourceParams.
void BuildTimelineLanes(UiState *ui, SceneState *scene, const TimelineLayout &layout) {
    for (int lane = 0; lane < layout.laneCount; ++lane) {
        float y = layout.lanesY + lane * kTimelineLaneHeight;
        PushRect(ui, {layout.rulerX, y, layout.rulerRight - layout.rulerX, kTimelineLaneHeight},
                 (lane % 2) == 0 ? theme::TimelineLaneEven : theme::TimelineLaneOdd);

        Rect header = {layout.rulerX - kTimelineHeaderWidth, y, kTimelineHeaderWidth,
                       kTimelineLaneHeight};
        PushRect(ui, header, theme::TimelineLaneHeader);

        EntityId source = layout.laneInfo[lane].source;
        int nameId = kTimelineTrackCtlBase + lane * 4 + 0;
        int volId = kTimelineTrackCtlBase + lane * 4 + 1;
        int muteId = kTimelineTrackCtlBase + lane * 4 + 2;
        int soloId = kTimelineTrackCtlBase + lane * 4 + 3;

        Rect nameRect = {header.x + kTimelineHeaderPad, y + kTimelineHeaderPad,
                         header.w - kTimelineHeaderPad * 2.0f, kTimelineHeaderRowH};
        if (ui->focusedInputId == nameId) {
            char newName[64];
            if (TextInputWidget(ui, nameRect, newName, sizeof(newName)) == UiEdit_Commit &&
                newName[0] != '\0') {
                SceneRenameEntity(scene, source, newName);
            }
        } else {
            PushText(ui, nameRect.x, nameRect.y, layout.laneInfo[lane].name, kTextCol);
            if (ui->mousePressed && PointInRect(ui->mouseX, ui->mouseY, nameRect)) {
                bool doubleClick = ui->lastClickControlId == nameId &&
                                   (ui->frameIndex - ui->lastClickFrame) < 18;
                ui->lastClickControlId = nameId;
                ui->lastClickFrame = ui->frameIndex;
                if (doubleClick) {
                    BeginTextEdit(ui, nameId, layout.laneInfo[lane].name);
                }
            }
        }

        float rowY = y + kTimelineHeaderPad + kTimelineHeaderRowH + kTimelineHeaderRowGap;
        float toggleW = 22.0f;
        float toggleGap = 4.0f;
        Rect soloRect = {header.x + header.w - kTimelineHeaderPad - toggleW, rowY, toggleW,
                         kTimelineHeaderRowH};
        Rect muteRect = {soloRect.x - toggleGap - toggleW, rowY, toggleW, kTimelineHeaderRowH};
        float volLeft = header.x + kTimelineHeaderPad;
        Rect volRect = {volLeft, rowY, muteRect.x - toggleGap - volLeft, kTimelineHeaderRowH};

        AudioSourceParams *params = SceneFindAudioSourceParams(scene, source);
        if (params != nullptr) {
            TimelineHeaderSlider(ui, volId, volRect, &params->gain);
            if (TimelineHeaderToggle(ui, muteId, muteRect, "M", params->mute)) {
                params->mute = !params->mute;
            }
            if (TimelineHeaderToggle(ui, soloId, soloRect, "S", params->solo)) {
                params->solo = !params->solo;
            }
        }
    }
}

void BuildTimelineClips(UiState *ui, TimelineState *timeline, SceneState *scene,
                        const TimelineLayout &layout) {
    static TimelineClipRow clipRows[kMaxTimelineClips];
    int clipCount = TimelineClips(timeline, clipRows, kMaxTimelineClips);
    for (int i = 0; i < clipCount; ++i) {
        TimelineClipId id = clipRows[i].id;
        const TimelineClip &clip = clipRows[i].clip;
        int lane = TimelineLaneOfTrack(layout, clip.track);
        if (lane < 0) {
            continue;
        }

        double startTime = clip.startTime;
        double duration = clip.duration;
        int drawLane = lane;
        bool dragging = ui->timelineDragClip == id && ui->timelineDragMode != TimelineDrag_None;
        if (dragging) {
            double delta =
                (double)(ui->mouseX - ui->timelineDragGrabX) / layout.pixelsPerSecond;
            if (ui->timelineDragMode == TimelineDrag_Move) {
                startTime = ui->timelineDragStartTime + delta;
                if (startTime < 0.0) {
                    startTime = 0.0;
                }
                int hoverLane = TimelineLaneAtY(layout, ui->mouseY);
                if (hoverLane >= 0) {
                    drawLane = hoverLane;
                }
            } else if (ui->timelineDragMode == TimelineDrag_TrimRight) {
                duration = ui->timelineDragDuration + delta;
                if (duration < kTimelineMinClipSeconds) {
                    duration = kTimelineMinClipSeconds;
                }
            } else {
                double shift = delta;
                double maxShift = ui->timelineDragDuration - kTimelineMinClipSeconds;
                if (shift > maxShift) {
                    shift = maxShift;
                }
                if (shift < -ui->timelineDragTrimIn) {
                    shift = -ui->timelineDragTrimIn;
                }
                startTime = ui->timelineDragStartTime + shift;
                duration = ui->timelineDragDuration - shift;
            }
        }

        float laneY = layout.lanesY + drawLane * kTimelineLaneHeight;
        Rect r = {TimelineTimeToX(layout, startTime), laneY + 3.0f,
                  (float)(duration * layout.pixelsPerSecond), kTimelineLaneHeight - 6.0f};

        bool selected = SceneSelectionContains(scene, TimelineClipSelectionItem(id));
        if (ui->activeId == 0 && ui->mousePressed && PointInRect(ui->mouseX, ui->mouseY, r)) {
            SelectionItem item = TimelineClipSelectionItem(id);
            SceneSelectionSet(scene, &item, 1);
            selected = true;

            ui->activeId = kTimelineClipIdBase + i;
            ui->timelineDragClip = id;
            ui->timelineDragGrabX = ui->mouseX;
            ui->timelineDragStartTime = clip.startTime;
            ui->timelineDragDuration = clip.duration;
            ui->timelineDragTrimIn = clip.trimIn;
            ui->timelineDragTrimOut = clip.trimOut;
            if (ui->mouseX < r.x + kTimelineTrimHandleWidth) {
                ui->timelineDragMode = TimelineDrag_TrimLeft;
            } else if (ui->mouseX > r.x + r.w - kTimelineTrimHandleWidth) {
                ui->timelineDragMode = TimelineDrag_TrimRight;
            } else {
                ui->timelineDragMode = TimelineDrag_Move;
            }
        }

        PushClippedRect(ui, r, layout.rulerX, layout.rulerRight,
                        selected ? theme::TimelineClipSelected : theme::TimelineClip);
        PushClippedRect(ui, {r.x, r.y, kTimelineTrimHandleWidth, r.h}, layout.rulerX,
                        layout.rulerRight, theme::TimelineClipTrimHandle);
        PushClippedRect(ui, {r.x + r.w - kTimelineTrimHandleWidth, r.y, kTimelineTrimHandleWidth,
                             r.h},
                        layout.rulerX, layout.rulerRight, theme::TimelineClipTrimHandle);

        if (dragging && ui->mouseReleased) {
            if (ui->timelineDragMode == TimelineDrag_Move) {
                TimelineMoveClip(timeline, scene, id, layout.laneTracks[drawLane], startTime);
            } else if (ui->timelineDragMode == TimelineDrag_TrimRight) {
                double trimmed = ui->timelineDragDuration - duration;
                TimelineTrimClip(timeline, scene, id, ui->timelineDragTrimIn,
                                       ui->timelineDragTrimOut + trimmed, duration);
            } else {
                double shift = startTime - ui->timelineDragStartTime;
                TimelineTrimClip(timeline, scene, id, ui->timelineDragTrimIn + shift,
                                       ui->timelineDragTrimOut, duration);
            }
            ui->timelineDragClip = kInvalidTimelineClipId;
            ui->timelineDragMode = TimelineDrag_None;
        }
    }
}

void DrawTimelinePlayhead(UiState *ui, TimelineState *timeline, const TimelineLayout &layout) {
    float x = TimelineTimeToX(layout, TimelineTime(timeline));
    if (x < layout.rulerX || x > layout.rulerRight) {
        return;
    }
    float bottom = layout.lanesY + layout.laneCount * kTimelineLaneHeight;
    PushRect(ui, {x, layout.ruler.y, 2.0f, bottom - layout.ruler.y}, theme::Playhead);
}

// Ghost of where a hovered .wav drag would drop: a translucent clip block at
// the snapped time on the target lane (or a "new track" row past the last
// lane) plus an insertion line. No model change; uses the platform's
// drag-hover fields.
void DrawTimelineDropGhost(UiState *ui, FrameInput input, const TimelineLayout &layout) {
    if (!input.dragHovering) {
        return;
    }
    float hx = input.dragHoverX;
    float hy = input.dragHoverY;
    if (hx < layout.rulerX || hx > layout.rulerRight || hy < layout.ruler.y ||
        hy > layout.bodyBottom) {
        return;
    }

    int lane = TimelineLaneAtY(layout, hy);
    float laneY = layout.lanesY + (float)(lane >= 0 ? lane : layout.laneCount) * kTimelineLaneHeight;
    PushClippedRect(ui, {layout.rulerX, laneY, layout.rulerRight - layout.rulerX,
                         kTimelineLaneHeight},
                    layout.rulerX, layout.rulerRight, theme::TimelineLaneHeader);

    float x = TimelineTimeToX(layout, TimelineXToTime(layout, hx));
    float ghostW = 2.0f * layout.pixelsPerSecond;
    int files = input.dragHoverFileCount > 0 ? input.dragHoverFileCount : 1;
    for (int i = 0; i < files; ++i) {
        Rect g = {x + (float)i * ghostW, laneY + 3.0f, ghostW - 2.0f, kTimelineLaneHeight - 6.0f};
        PushClippedRect(ui, g, layout.rulerX, layout.rulerRight, theme::TimelineClipGhost);
    }
    PushRect(ui, {x, layout.ruler.y, 2.0f, layout.bodyBottom - layout.ruler.y}, theme::Playhead);
}

// A .wav dropped on a lane becomes a clip there; dropped past the last lane it
// gets a fresh track, bound to the selected audio source when there is one.
void HandleTimelineDrop(UiState *ui, FrameInput input, const TimelineLayout &layout) {
    if (input.droppedFileCount <= 0 || ui->editor.audio == nullptr) {
        return;
    }
    if (input.dropX < layout.rulerX || input.dropX > layout.rulerRight) {
        return;
    }
    if (input.dropY < layout.lanesY || input.dropY > layout.bodyBottom) {
        return;
    }

    int lane = TimelineLaneAtY(layout, input.dropY);
    TrackId track = lane >= 0 ? layout.laneTracks[lane] : SelectedAudioSource(ui->editor.scene);
    if (track == kInvalidTrackId) {
        return; // a clip needs a source; there is no lane and none selected
    }
    double dropTime = TimelineXToTime(layout, input.dropX);

    for (int i = 0; i < input.droppedFileCount; ++i) {
        WavId wav = AudioLoadWav(ui->editor.audio, input.droppedFiles[i]);
        if (!WavIdValid(wav)) {
            continue;
        }
        double duration = AudioWavDuration(ui->editor.audio, wav);
        TimelineAddClip(ui->editor.timeline, ui->editor.scene, track, wav, dropTime,
                                 duration);
        dropTime += duration;
    }
}

void BuildTimelineBody(UiState *ui, FrameInput input) {
    TimelineState *timeline = ui->editor.timeline;
    SceneState *scene = ui->editor.scene;
    if (ui->suppressBody || timeline == nullptr || scene == nullptr) {
        return;
    }

    Rect body = ui->currentBody;
    float lanesTop = body.y + kTimelineTransportHeight + kRowGap + kTimelineRulerHeight;
    if (body.w < kTimelineHeaderWidth + 120.0f || body.h < kTimelineTransportHeight) {
        return;
    }

    static TimelineTrackRow trackRows[kMaxTimelineTracks];
    int trackCount = TimelineTracks(timeline, scene, trackRows, kMaxTimelineTracks);
    float lanesViewH = (body.y + body.h) - lanesTop;
    float lanesFullH = (float)trackCount * kTimelineLaneHeight;
    float maxLaneScroll = lanesFullH > lanesViewH ? lanesFullH - lanesViewH : 0.0f;

    // Scroll while the pointer is over the panel. Horizontal (trackpad swipe)
    // always pans time. Vertical scrolls the lanes when they overflow, else
    // also pans time; Shift forces time; Alt zooms about the cursor.
    bool overBody = PointInRect(ui->mouseX, ui->mouseY, body);
    if (overBody) {
        float rulerX = body.x + kTimelineHeaderWidth;
        if (input.alt && (input.scrollX != 0.0f || input.scrollY != 0.0f)) {
            float w = input.scrollY != 0.0f ? input.scrollY : input.scrollX;
            double cursorTime = ui->timelineScrollSeconds +
                                (double)(ui->mouseX - rulerX) / ui->timelinePixelsPerSecond;
            ui->timelinePixelsPerSecond =
                Clamp(ui->timelinePixelsPerSecond * expf(w * 0.015f),
                      kTimelineMinPixelsPerSecond, kTimelineMaxPixelsPerSecond);
            ui->timelineScrollSeconds =
                cursorTime - (double)(ui->mouseX - rulerX) / ui->timelinePixelsPerSecond;
        } else if (input.shift && input.scrollY != 0.0f) {
            ui->timelineScrollSeconds -= (double)input.scrollY / ui->timelinePixelsPerSecond;
        } else {
            if (input.scrollX != 0.0f) {
                ui->timelineScrollSeconds -= (double)input.scrollX / ui->timelinePixelsPerSecond;
            }
            if (input.scrollY != 0.0f) {
                ui->timelineLaneScroll -= input.scrollY; // vertical only scrolls lanes
            }
        }
    }
    if (ui->timelineScrollSeconds < 0.0) {
        ui->timelineScrollSeconds = 0.0;
    }
    ui->timelineLaneScroll = Clamp(ui->timelineLaneScroll, 0.0f, maxLaneScroll);

    TimelineLayout layout;
    layout.rulerX = body.x + kTimelineHeaderWidth;
    layout.rulerRight = body.x + body.w;
    layout.ruler = {layout.rulerX, body.y + kTimelineTransportHeight + kRowGap,
                    layout.rulerRight - layout.rulerX, kTimelineRulerHeight};
    layout.lanesY = lanesTop - ui->timelineLaneScroll;
    layout.bodyBottom = body.y + body.h;
    layout.pixelsPerSecond = ui->timelinePixelsPerSecond;
    layout.scrollSeconds = ui->timelineScrollSeconds;

    int visibleLanes = (int)(lanesViewH / kTimelineLaneHeight) + 2;
    if (visibleLanes < 0) {
        visibleLanes = 0;
    }
    layout.laneCount = trackCount < visibleLanes ? trackCount : visibleLanes;
    for (int i = 0; i < layout.laneCount; ++i) {
        layout.laneTracks[i] = trackRows[i].id;
        layout.laneInfo[i] = trackRows[i].track;
    }

    BuildTimelineTransport(ui, timeline, {body.x, body.y, body.w, kTimelineTransportHeight});
    BuildTimelineRuler(ui, timeline, layout);
    BuildTimelineLanes(ui, scene, layout);
    BuildTimelineClips(ui, timeline, scene, layout);
    DrawTimelinePlayhead(ui, timeline, layout);
    DrawTimelineDropGhost(ui, input, layout);
    HandleTimelineDrop(ui, input, layout);

    if (ui->timelineDragMode != TimelineDrag_None && !ui->mouseDown) {
        ui->timelineDragClip = kInvalidTimelineClipId; // the clip went away mid-drag
        ui->timelineDragMode = TimelineDrag_None;
    }
}

// ---- scene outliner (right panel) --------------------------------------

void BeginTextEdit(UiState *ui, int id, const char *initial) {
    ui->focusedInputId = id;
    strncpy(ui->editBuffer, initial, sizeof(ui->editBuffer) - 1);
    ui->editBuffer[sizeof(ui->editBuffer) - 1] = '\0';
    ui->editLen = (int)strlen(ui->editBuffer);
    ui->editCaret = ui->editLen;
}

// Only called while ui->focusedInputId == id (start it with BeginTextEdit).
// Consumes this frame's key events, renders the buffer + a blinking caret, and
// returns UiEdit_Commit (Return / click-away, `out` filled) or UiEdit_Cancel
// (Esc). Clears focus on either.
int TextInputWidget(UiState *ui, Rect r, char *out, int outCap) {
    int result = UiEdit_None;
    for (int i = 0; i < ui->frameKeyCount && result == UiEdit_None; ++i) {
        const KeyEvent &key = ui->frameKeys[i];
        if (!key.pressed) {
            continue;
        }
        unsigned int cp = key.codepoint;
        if (key.keyCode == 36 || cp == 13) { // Return
            result = UiEdit_Commit;
        } else if (key.keyCode == 53 || cp == 27) { // Esc
            result = UiEdit_Cancel;
        } else if (cp == 127 || key.keyCode == 51) { // Backspace
            if (ui->editCaret > 0) {
                memmove(ui->editBuffer + ui->editCaret - 1, ui->editBuffer + ui->editCaret,
                        (size_t)(ui->editLen - ui->editCaret + 1));
                --ui->editCaret;
                --ui->editLen;
            }
        } else if (key.keyCode == 123) { // Left
            if (ui->editCaret > 0) --ui->editCaret;
        } else if (key.keyCode == 124) { // Right
            if (ui->editCaret < ui->editLen) ++ui->editCaret;
        } else if (cp >= 32 && cp < 127 && ui->editLen < (int)sizeof(ui->editBuffer) - 1) {
            memmove(ui->editBuffer + ui->editCaret + 1, ui->editBuffer + ui->editCaret,
                    (size_t)(ui->editLen - ui->editCaret + 1));
            ui->editBuffer[ui->editCaret] = (char)cp;
            ++ui->editCaret;
            ++ui->editLen;
        }
    }
    if (result == UiEdit_None && ui->mousePressed && !PointInRect(ui->mouseX, ui->mouseY, r)) {
        result = UiEdit_Commit;
    }

    PushRect(ui, r, kTrackCol);
    PushBorder(ui, r, 1.0f, kResizeGripHot);
    float textY = r.y + (r.h - TextLineHeight()) * 0.5f;
    PushText(ui, r.x + 5.0f, textY, ui->editBuffer, kTextCol);
    if ((ui->frameIndex / 30) % 2 == 0) {
        float caretX = r.x + 5.0f + TextWidthN(ui->editBuffer, ui->editCaret);
        PushRect(ui, {caretX, r.y + 4.0f, 2.0f, r.h - 8.0f}, kTextCol);
    }

    if (result == UiEdit_Commit) {
        strncpy(out, ui->editBuffer, (size_t)outCap - 1);
        out[outCap - 1] = '\0';
    }
    if (result != UiEdit_None) {
        ui->focusedInputId = 0;
    }
    return result;
}

const char *EntityKindGlyph(EntityKind kind) {
    if (kind == EntityKind_AudioSource) return "S";
    if (kind == EntityKind_AudioListener) return "L";
    return "M";
}

constexpr int kOutlinerAddId = 1000900;
constexpr int kOutlinerRenameIdBase = 1002000;
constexpr float kOutlinerRowHeight = 22.0f;

void BuildOutliner(UiState *ui) {
    SceneState *scene = ui->editor.scene;
    if (ui->suppressBody || scene == nullptr) {
        return;
    }

    Rect body = ui->currentBody;
    float x = body.x;
    float y = ui->panelCursorY;

    Rect addRect = {x, y, 70.0f, kButtonHeight};
    y += kButtonHeight + kRowGap;
    if (ui->editor.requestAddMenu) {
        ui->addMenuOpen = true;
    }
    if (Button(ui, kOutlinerAddId, addRect, "+ Add")) {
        ui->addMenuOpen = !ui->addMenuOpen;
    }

    if (ui->addMenuOpen) {
        struct KindOpt {
            const char *label;
            int kind;
            int mesh;
        };
        const KindOpt opts[] = {
            {"Cube", EntityKind_Mesh, MeshId_Cube},
            {"Plane", EntityKind_Mesh, MeshId_Plane},
            {"Source", EntityKind_AudioSource, 0},
            {"Listener", EntityKind_AudioListener, 0},
        };
        Rect menuRect = {addRect.x, addRect.y + addRect.h, 150.0f, 4.0f * kMenuRowHeight};
        PushRect(ui, menuRect, kMenuDropdownBg);
        PushBorder(ui, menuRect, 1.0f, kPanelBorderCol);
        bool pickedOrOutside = false;
        for (int i = 0; i < 4; ++i) {
            Rect row = {menuRect.x, menuRect.y + (float)i * kMenuRowHeight, menuRect.w,
                        kMenuRowHeight};
            bool hot = PointInRect(ui->mouseX, ui->mouseY, row);
            if (hot) {
                PushRect(ui, row, kMenuItemHot);
            }
            PushText(ui, row.x + 10.0f,
                     row.y + (kMenuRowHeight - TextLineHeight()) * 0.5f, opts[i].label,
                     kTextCol);
            if (hot && ui->mousePressed) {
                Transform t = TransformIdentity();
                if (opts[i].kind == EntityKind_Mesh) {
                    SceneCreateMeshEntityAt(scene, opts[i].label, (MeshId)opts[i].mesh, t);
                } else {
                    SceneCreateEntityAt(scene, (EntityKind)opts[i].kind, opts[i].label, t);
                }
                ui->addMenuOpen = false;
                pickedOrOutside = true;
            }
        }
        if (!pickedOrOutside && ui->mousePressed && !PointInRect(ui->mouseX, ui->mouseY, menuRect) &&
            !PointInRect(ui->mouseX, ui->mouseY, addRect)) {
            ui->addMenuOpen = false;
        }
        y += 4.0f * kMenuRowHeight + kRowGap;
    }

    SceneEntityRow rows[256];
    int count = SceneEntities(scene, rows, 256);
    for (int i = 0; i < count && y + kOutlinerRowHeight <= body.y + body.h; ++i) {
        Rect row = {x, y, body.w, kOutlinerRowHeight};
        y += kOutlinerRowHeight + 2.0f;

        SelectionItem item = {SelectionKind_Entity, (uint32_t)rows[i].id};
        if (SceneSelectionContains(scene, item)) {
            PushRect(ui, row, kButtonActive);
        }

        float glyphY = row.y + (kOutlinerRowHeight - TextLineHeight()) * 0.5f;
        PushText(ui, row.x + 2.0f, glyphY, EntityKindGlyph(rows[i].kind), kTextShortcut);

        int renameId = kOutlinerRenameIdBase + i;
        Rect nameRect = {row.x + 20.0f, row.y, row.w - 22.0f, kOutlinerRowHeight};

        if (ui->focusedInputId == renameId) {
            char newName[64];
            if (TextInputWidget(ui, nameRect, newName, sizeof(newName)) == UiEdit_Commit &&
                newName[0] != '\0') {
                SceneRenameEntity(scene, rows[i].id, newName);
            }
        } else {
            PushText(ui, nameRect.x + 3.0f, glyphY, rows[i].name, kTextCol);
            if (ui->mousePressed && PointInRect(ui->mouseX, ui->mouseY, row)) {
                bool doubleClick = ui->lastClickControlId == renameId &&
                                   (ui->frameIndex - ui->lastClickFrame) < 18;
                ui->lastClickControlId = renameId;
                ui->lastClickFrame = ui->frameIndex;
                if (doubleClick) {
                    BeginTextEdit(ui, renameId, rows[i].name);
                } else {
                    SceneSelectionSet(scene, &item, 1);
                }
            }
        }
    }

    ui->panelCursorY = y;
}

void DrawToolTooltip(UiState *ui, Rect button, const char *label, const char *shortcut) {
    char text[48];
    snprintf(text, sizeof(text), "%s  (%s)", label, shortcut);
    float pad = 6.0f;
    Rect box = {button.x, button.y + button.h + 6.0f, TextWidth(text) + pad * 2.0f,
                TextLineHeight() + pad * 2.0f};
    PushRect(ui, box, theme::ToolbarBg);
    PushBorder(ui, box, 1.0f, kPanelBorderCol);
    PushText(ui, box.x + pad, box.y + pad, text, kTextCol);
}

void BuildToolbar(UiState *ui) {
    ui->toolbarRect = {0.0f, 0.0f, 0.0f, 0.0f};
    Rect content = ui->contentRect;
    if (content.w < 240.0f || content.h < 120.0f) {
        return; // viewport too small to overlay a toolbar
    }

    float widths[kToolButtonCount];
    float barWidth = kToolbarInsetX * 2.0f;
    for (int i = 0; i < kToolButtonCount; ++i) {
        widths[i] = TextWidth(kToolButtons[i].label) + 16.0f;
        barWidth += widths[i] + (i > 0 ? kToolButtonGap : 0.0f);
    }

    Rect bar = {content.x + kToolbarMargin, content.y + kToolbarMargin, barWidth,
                kToolButtonHeight + kToolbarInsetY * 2.0f};
    PushRect(ui, bar, theme::ToolbarBg);

    float penX = bar.x + kToolbarInsetX;
    float btnY = bar.y + kToolbarInsetY;
    for (int i = 0; i < kToolButtonCount; ++i) {
        const ToolButtonDef &def = kToolButtons[i];
        Rect r = {penX, btnY, widths[i], kToolButtonHeight};
        penX += widths[i] + kToolButtonGap;
        int buttonId = 700000 + i;
        bool on = ToolButtonOn(def, &ui->editor);
        if (ToolButtonWidget(ui, buttonId, r, def.label, on) && ui->editor.scene != nullptr) {
            SceneSetToolMode(ui->editor.scene, def.mode);
        }
        if (ui->hotId == buttonId) {
            DrawToolTooltip(ui, r, def.label, def.shortcut);
        }
    }

    ui->toolbarRect = bar;
}

} // namespace

UiState *UiInit(Arena *arena, float drawableWidth, float drawableHeight) {
    UiState *ui = ArenaPushStruct(arena, UiState);
    ui->uiScale = 1.0f;
    ui->realDrawableWidth = drawableWidth;
    ui->realDrawableHeight = drawableHeight;
    ui->drawableWidth = drawableWidth;
    ui->drawableHeight = drawableHeight;
    ui->timelinePixelsPerSecond = 40.0f;
    ui->menuBar = MenuBarDefault();
    ui->openMenu = -1;
    ui->contentRect = {0.0f, 0.0f, drawableWidth, drawableHeight};
    return ui;
}

void UiHandleResize(UiState *ui, float drawableWidth, float drawableHeight) {
    ui->realDrawableWidth = drawableWidth;
    ui->realDrawableHeight = drawableHeight;
    ui->drawableWidth = drawableWidth / ui->uiScale;
    ui->drawableHeight = drawableHeight / ui->uiScale;
    for (int i = 0; i < kMaxPanels; ++i) {
        if (ui->panels[i].id != 0 && ui->panels[i].dock == UiDock_Float) {
            ui->panels[i].floatRect = ClampFloatRect(ui, ui->panels[i].floatRect);
        }
    }
}

void UiSetUiScale(UiState *ui, float scale) {
    ui->uiScale = Clamp(scale, 0.6f, 2.5f);
    UiHandleResize(ui, ui->realDrawableWidth, ui->realDrawableHeight);
}

void UiAdjustUiScale(UiState *ui, float delta) { UiSetUiScale(ui, ui->uiScale + delta); }

float UiUiScale(const UiState *ui) { return ui->uiScale; }

void UiSetFontMetrics(UiState *ui, const UiFontMetrics *metrics) {
    (void)ui;
    g_font = metrics;
}

void UiBuildFrame(UiState *ui, FrameInput input, CommandContext menuContext) {
    ui->vertexCount = 0;
    ui->menuContext = menuContext;

    // Everything below works in logical units; bring the platform's real-pixel
    // pointer/drop coordinates into that space.
    float invScale = 1.0f / ui->uiScale;
    input.mouseX *= invScale;
    input.mouseY *= invScale;
    input.dropX *= invScale;
    input.dropY *= invScale;
    input.dragHoverX *= invScale;
    input.dragHoverY *= invScale;

    ui->mouseX = input.mouseX;
    ui->mouseY = input.mouseY;
    ui->mouseDown = input.mouseLeftDown;
    ui->mousePressed = input.mouseLeftDown && !ui->prevMouseDown;
    ui->mouseReleased = !input.mouseLeftDown && ui->prevMouseDown;
    ui->hotId = 0;
    ui->frameKeys = input.keyEvents;
    ui->frameKeyCount = input.keyEventCount;
    ++ui->frameIndex;

    ui->menuBarVisible = input.fullscreen || menuContext.menuState->showMenuBar;
    ui->menuBarHeight = ui->menuBarVisible ? kMenuBarHeight : 0.0f;
    MenuUpdate(ui);
    if (ui->menuHasPointer) {
        ui->mousePressed = false;
    }

    ResolvePanelLayout(ui);
    UpdatePanelInteraction(ui);
    ResolvePanelLayout(ui); // reflect a drag/resize that started or ended this frame

    UiBeginPanel(ui, 1, "Objects", UiDock_Right, 264.0f);
    BuildOutliner(ui);
    UiEndPanel(ui);

    UiBeginPanel(ui, 2, "Scene", UiDock_Left, 208.0f);
    char line[48];
    snprintf(line, sizeof(line), "View %d x %d", (int)ui->contentRect.w, (int)ui->contentRect.h);
    UiPanelText(ui, line);
    snprintf(line, sizeof(line), "Win  %d x %d", (int)ui->drawableWidth, (int)ui->drawableHeight);
    UiPanelText(ui, line);
    UiPanelButton(ui, "Reset Camera");
    UiEndPanel(ui);

    UiBeginPanel(ui, 3, "Timeline", UiDock_Bottom, 300.0f);
    BuildTimelineBody(ui, input);
    UiEndPanel(ui);

    if (ui->draggedPanel != 0) {
        PushBorder(ui, ui->dragOutline, 2.0f, kDragOutlineCol);
    }

    if (ui->marqueeActive) {
        PushRect(ui, ui->marqueeRect, theme::MarqueeFill);
        PushBorder(ui, ui->marqueeRect, 1.0f, kDragOutlineCol);
    }

    BuildToolbar(ui);

    MenuDraw(ui);

    if (ui->mouseReleased) {
        ui->activeId = 0;
    }
    ui->prevMouseDown = ui->mouseDown;
}

void UiBeginPanel(UiState *ui, UiPanelId id, const char *title, int initialDock, float initialSize) {
    int slot = PanelSlotOrCreate(ui, id, initialDock, initialSize);
    ui->currentPanel = id;
    ui->controlIndex = 0;
    ui->suppressBody = (id == ui->draggedPanel);
    if (ui->suppressBody) {
        return; // torn off this frame: no chrome, no body; the outline stands in
    }

    Rect r = ui->panelRects[slot];
    if (r.w <= 0.0f || r.h <= 0.0f) {
        ui->suppressBody = true;
        return;
    }

    PushRect(ui, r, kPanelBg);

    Rect titleBar = {r.x, r.y, r.w, kTitleBarHeight};
    bool titleHot = ui->draggedPanel == 0 && ui->resizePanel == 0 &&
                    PointInRect(ui->mouseX, ui->mouseY, titleBar);
    PushRect(ui, titleBar, titleHot ? kTitleBarHot : kTitleBarCol);
    PushText(ui, r.x + 8.0f, r.y + (kTitleBarHeight - TextLineHeight()) * 0.5f, title,
             kTextCol);

    Rect grip = ResizeGripRect(ui, slot);
    if (grip.w > 0.0f) {
        bool gripHot = ui->resizePanel == ui->panels[slot].id ||
                       PointInRect(ui->mouseX, ui->mouseY, grip);
        PushRect(ui, grip, gripHot ? kResizeGripHot : kResizeGripCol);
    }

    PushBorder(ui, r, kPanelBorderPx, kPanelBorderCol);

    ui->currentBody = {r.x + kPadding, r.y + kTitleBarHeight + kPadding, r.w - kPadding * 2.0f,
                       r.h - kTitleBarHeight - kPadding * 2.0f};
    ui->panelCursorY = ui->currentBody.y;
}

void UiEndPanel(UiState *ui) {
    ui->currentPanel = 0;
    ui->suppressBody = false;
}

bool UiPanelButton(UiState *ui, const char *label) {
    int id = ui->currentPanel * 1000 + (++ui->controlIndex);
    if (ui->suppressBody) {
        return false;
    }
    Rect r = {ui->currentBody.x, ui->panelCursorY, ui->currentBody.w, kButtonHeight};
    ui->panelCursorY += kButtonHeight + kRowGap;
    return Button(ui, id, r, label);
}

void UiPanelSlider(UiState *ui, const char *label, float *value) {
    int id = ui->currentPanel * 1000 + (++ui->controlIndex);
    if (ui->suppressBody) {
        return;
    }
    Rect r = {ui->currentBody.x, ui->panelCursorY, ui->currentBody.w, kSliderHeight};
    ui->panelCursorY += kSliderHeight + kRowGap;
    Slider(ui, id, r, label, value);
}

void UiPanelText(UiState *ui, const char *text) {
    ++ui->controlIndex;
    if (ui->suppressBody) {
        return;
    }
    PushText(ui, ui->currentBody.x, ui->panelCursorY, text, kTextCol);
    ui->panelCursorY += TextLineHeight() + kRowGap;
}

CommandId UiTakeCommand(UiState *ui) {
    CommandId command = ui->pendingCommand;
    ui->pendingCommand = Command_None;
    return command;
}

void UiSetEditorState(UiState *ui, UiEditorState editor) {
    ui->editor = editor;
}

void UiSetMarquee(UiState *ui, bool active, float x0, float y0, float x1, float y1) {
    ui->marqueeActive = active;
    float s = 1.0f / ui->uiScale; // caller passes real pixels; store logical
    x0 *= s;
    y0 *= s;
    x1 *= s;
    y1 *= s;
    float minX = x0 < x1 ? x0 : x1;
    float minY = y0 < y1 ? y0 : y1;
    ui->marqueeRect = {minX, minY, fabsf(x1 - x0), fabsf(y1 - y0)};
}

bool UiWantsMouse(const UiState *ui) {
    if (ui->menuHasPointer || ui->openMenu >= 0) {
        return true;
    }
    if (ui->draggedPanel != 0 || ui->resizePanel != 0 || ui->activeId != 0 ||
        ui->focusedInputId != 0) {
        return true;
    }
    if (ui->toolbarRect.w > 0.0f && PointInRect(ui->mouseX, ui->mouseY, ui->toolbarRect)) {
        return true;
    }
    for (int i = 0; i < kMaxPanels; ++i) {
        if (ui->panels[i].id != 0 && PointInRect(ui->mouseX, ui->mouseY, ui->panelRects[i])) {
            return true;
        }
    }
    return false;
}

bool UiWantsKeyboard(const UiState *ui) {
    return ui->openMenu >= 0 || ui->focusedInputId != 0;
}

// The content rect and drawable size are consumed by the renderer / platform
// in real drawable pixels, so scale the logical layout values back up.
float UiContentOriginX(const UiState *ui) {
    return ui->contentRect.x * ui->uiScale;
}

float UiContentOriginY(const UiState *ui) {
    return ui->contentRect.y * ui->uiScale;
}

float UiContentWidth(const UiState *ui) {
    float w = ui->contentRect.w * ui->uiScale;
    return w < 1.0f ? 1.0f : w;
}

float UiContentHeight(const UiState *ui) {
    float h = ui->contentRect.h * ui->uiScale;
    return h < 1.0f ? 1.0f : h;
}

float UiDrawableWidth(const UiState *ui) {
    return ui->realDrawableWidth;
}

float UiDrawableHeight(const UiState *ui) {
    return ui->realDrawableHeight;
}

const UiVertex *UiVertices(const UiState *ui) {
    return ui->vertices;
}

int UiVertexCount(const UiState *ui) {
    return ui->vertexCount;
}
