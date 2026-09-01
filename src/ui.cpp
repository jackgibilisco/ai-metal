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
constexpr float kTextScale = 2.0f;
constexpr float kGlyphPixels = 8.0f; // font cell is 8x8; advance and line height
constexpr float kGlyphHeight = kGlyphPixels;

// Must match the atlas built in ui_render_metal.mm: glyph `c` occupies cell
// (c - kFontFirstChar) of a kFontCharCount-wide row.
constexpr int kFontFirstChar = 32;
constexpr int kFontCharCount = 96;

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
constexpr float kPanelMinDockSize = 150.0f;
constexpr float kDockMaxFraction = 0.6f;
constexpr float kFloatMinW = 170.0f;
constexpr float kFloatMinH = 90.0f;

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

    UiVertex vertices[kMaxVertices];
    int vertexCount;
};

namespace {

void PushVertexUV(UiState *ui, float x, float y, float u, float v, float mode, Color c) {
    assert(ui->vertexCount < kMaxVertices);
    UiVertex &vert = ui->vertices[ui->vertexCount++];
    vert.x = x;
    vert.y = y;
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

void PushGlyph(UiState *ui, float x, float y, float u0, float u1, Color c) {
    float x1 = x + kGlyphPixels * kTextScale;
    float y1 = y + kGlyphPixels * kTextScale;
    PushVertexUV(ui, x, y, u0, 0.0f, 1.0f, c);
    PushVertexUV(ui, x1, y, u1, 0.0f, 1.0f, c);
    PushVertexUV(ui, x1, y1, u1, 1.0f, 1.0f, c);
    PushVertexUV(ui, x, y, u0, 0.0f, 1.0f, c);
    PushVertexUV(ui, x1, y1, u1, 1.0f, 1.0f, c);
    PushVertexUV(ui, x, y1, u0, 1.0f, 1.0f, c);
}

void PushText(UiState *ui, float x, float y, const char *text, Color c) {
    float penX = x;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        int codepoint = *p;
        if (codepoint >= kFontFirstChar && codepoint < kFontFirstChar + kFontCharCount) {
            int cell = codepoint - kFontFirstChar;
            float u0 = (float)cell / (float)kFontCharCount;
            float u1 = (float)(cell + 1) / (float)kFontCharCount;
            PushGlyph(ui, penX, y, u0, u1, c);
        }
        penX += kGlyphPixels * kTextScale;
    }
}

float TextWidth(const char *text) {
    return (float)strlen(text) * kGlyphPixels * kTextScale;
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
    float textY = r.y + (r.h - kGlyphHeight * kTextScale) * 0.5f;
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

    float titleTextY = (kMenuBarHeight - kGlyphHeight * kTextScale) * 0.5f;
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
        float textY = row.y + (kMenuRowHeight - kGlyphHeight * kTextScale) * 0.5f;

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
        PanelState &p = ui->panels[i];
        if (p.id == 0 || p.id == ui->draggedPanel) {
            continue;
        }
        if (p.dock == UiDock_Float) {
            ui->panelRects[i] = ClampFloatRect(ui, p.floatRect);
            continue;
        }
        float size = Clamp(p.dockSize, kPanelMinDockSize, DockLimit(ui, p.dock));
        if (p.dock == UiDock_Left) {
            ui->panelRects[i] = {free.x, free.y, size, free.h};
            free.x += size;
            free.w -= size;
        } else if (p.dock == UiDock_Right) {
            ui->panelRects[i] = {free.x + free.w - size, free.y, size, free.h};
            free.w -= size;
        } else if (p.dock == UiDock_Top) {
            ui->panelRects[i] = {free.x, free.y, free.w, size};
            free.y += size;
            free.h -= size;
        } else {
            ui->panelRects[i] = {free.x, free.y + free.h - size, free.w, size};
            free.h -= size;
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
            outline = DockRect(ui, preview, Clamp(want, kPanelMinDockSize, DockLimit(ui, preview)));
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
            p.dockSize = Clamp(size, kPanelMinDockSize, DockLimit(ui, p.dock));
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

enum { ToolBtn_Mode, ToolBtn_Action, ToolBtn_Toggle };

struct ToolButtonDef {
    const char *label;
    int kind;
    int modeValue;                       // ToolBtn_Mode: value written into *editor.toolMode
    void (*run)(const UiEditorState *e);  // ToolBtn_Action / ToolBtn_Toggle
    bool (*isOn)(const UiEditorState *e); // extra highlight test (ToolBtn_Toggle)
};

void ToolRunAddSource(const UiEditorState *e) {
    if (e->addSource != nullptr) {
        e->addSource(e->context);
    }
}

void ToolRunAddListener(const UiEditorState *e) {
    if (e->addListener != nullptr) {
        e->addListener(e->context);
    }
}

void ToolRunFrameSelected(const UiEditorState *e) {
    if (e->frameSelected != nullptr) {
        e->frameSelected(e->context);
    }
}

void ToolRunToggleSnap(const UiEditorState *e) {
    if (e->snapEnabled != nullptr) {
        *e->snapEnabled = !*e->snapEnabled;
    }
}

bool ToolSnapOn(const UiEditorState *e) {
    return e->snapEnabled != nullptr && *e->snapEnabled;
}

const ToolButtonDef kToolButtons[] = {
    {"Select", ToolBtn_Mode, UiTool_Select, nullptr, nullptr},
    {"Translate", ToolBtn_Mode, UiTool_Translate, nullptr, nullptr},
    {"Rotate", ToolBtn_Mode, UiTool_Rotate, nullptr, nullptr},
    {"Scale", ToolBtn_Mode, UiTool_Scale, nullptr, nullptr},
    {"Add Source", ToolBtn_Action, 0, ToolRunAddSource, nullptr},
    {"Add Listener", ToolBtn_Action, 0, ToolRunAddListener, nullptr},
    {"Snap", ToolBtn_Toggle, 0, ToolRunToggleSnap, ToolSnapOn},
    {"Frame Selected", ToolBtn_Action, 0, ToolRunFrameSelected, nullptr},
};
constexpr int kToolButtonCount = (int)(sizeof(kToolButtons) / sizeof(kToolButtons[0]));

constexpr float kToolbarMargin = 10.0f;
constexpr float kToolbarInsetX = 12.0f;
constexpr float kToolButtonHeight = 26.0f;
constexpr float kToolButtonGap = 4.0f;
constexpr float kToolbarInsetY = 6.0f;

bool ToolButtonOn(const ToolButtonDef &def, const UiEditorState *e) {
    if (def.kind == ToolBtn_Mode) {
        return e->toolMode != nullptr && *e->toolMode == def.modeValue;
    }
    if (def.isOn != nullptr) {
        return def.isOn(e);
    }
    return false;
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
    float textY = r.y + (r.h - kGlyphHeight * kTextScale) * 0.5f;
    PushText(ui, textX, textY, label, kTextCol);
    return clicked;
}

constexpr float kTimelinePixelsPerSecond = 40.0f;
constexpr float kTimelineTransportHeight = 26.0f;
constexpr float kTimelineRulerHeight = 28.0f;
constexpr float kTimelineLaneHeight = 34.0f;
constexpr float kTimelineHeaderWidth = 150.0f;
constexpr float kTimelineTrimHandleWidth = 6.0f;
constexpr float kTimelinePlayheadGrabPx = 5.0f;
constexpr int kTimelineRulerLabelSeconds = 5;
constexpr double kTimelineMinClipSeconds = 0.05;

constexpr int kTimelinePlayId = 800010;
constexpr int kTimelineStopId = 800011;
constexpr int kTimelinePlayheadId = 800012;
constexpr int kTimelineClipIdBase = 810000;

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
    int laneCount;
    TrackId laneTracks[kMaxTimelineTracks];
    TimelineTrack laneInfo[kMaxTimelineTracks];
};

float TimelineTimeToX(float rulerX, double seconds) {
    return rulerX + (float)(seconds * kTimelinePixelsPerSecond);
}

double TimelineXToTime(float rulerX, float x) {
    double seconds = (double)(x - rulerX) / kTimelinePixelsPerSecond;
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
    const Entity *entity = SceneGetEntity(scene, active);
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
    PushText(ui, row.x + 178.0f, row.y + (row.h - kGlyphHeight * kTextScale) * 0.5f, readout,
             kTextCol);
}

void BuildTimelineRuler(UiState *ui, TimelineState *timeline, const TimelineLayout &layout) {
    PushRect(ui, layout.ruler, theme::TimelineRuler);

    double endTime = TimelineXToTime(layout.rulerX, layout.rulerRight);
    for (int second = 0; (double)second <= endTime; ++second) {
        float x = TimelineTimeToX(layout.rulerX, second);
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

    float playheadX = TimelineTimeToX(layout.rulerX, TimelineTime(timeline));
    Rect grab = {playheadX - kTimelinePlayheadGrabPx, layout.ruler.y,
                 kTimelinePlayheadGrabPx * 2.0f, layout.ruler.h};
    if (ui->activeId == 0 && ui->mousePressed) {
        if (PointInRect(ui->mouseX, ui->mouseY, grab)) {
            ui->activeId = kTimelinePlayheadId;
        } else if (PointInRect(ui->mouseX, ui->mouseY, layout.ruler)) {
            TimelineSeek(timeline, TimelineXToTime(layout.rulerX, ui->mouseX));
        }
    }

    if (ui->activeId == kTimelinePlayheadId) {
        TimelineScrub(timeline, TimelineXToTime(layout.rulerX, ui->mouseX));
        if (ui->mouseReleased) {
            TimelineScrubEnd(timeline);
        }
    }
}

void DrawTimelineLanes(UiState *ui, const TimelineLayout &layout) {
    for (int lane = 0; lane < layout.laneCount; ++lane) {
        float y = layout.lanesY + lane * kTimelineLaneHeight;
        PushRect(ui, {layout.rulerX, y, layout.rulerRight - layout.rulerX, kTimelineLaneHeight},
                 (lane % 2) == 0 ? theme::TimelineLaneEven : theme::TimelineLaneOdd);

        Rect header = {layout.rulerX - kTimelineHeaderWidth, y, kTimelineHeaderWidth,
                       kTimelineLaneHeight};
        PushRect(ui, header, theme::TimelineLaneHeader);

        float textY = y + (kTimelineLaneHeight - kGlyphHeight * kTextScale) * 0.5f;
        char name[7];
        strncpy(name, layout.laneInfo[lane].name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        PushText(ui, header.x + 6.0f, textY, name, kTextCol);

        bool muted = layout.laneInfo[lane].muted;
        bool soloed = layout.laneInfo[lane].soloed;
        Rect mute = {header.x + header.w - 42.0f, textY, 18.0f, 18.0f};
        Rect solo = {header.x + header.w - 21.0f, textY, 18.0f, 18.0f};
        PushRect(ui, mute, muted ? kButtonActive : kButtonCol);
        PushRect(ui, solo, soloed ? kButtonActive : kButtonCol);
        PushText(ui, mute.x + 1.0f, textY, "M", muted ? kTextCol : kTextDisabled);
        PushText(ui, solo.x + 1.0f, textY, "S", soloed ? kTextCol : kTextDisabled);
    }
}

void BuildTimelineClips(UiState *ui, TimelineState *timeline, SceneState *scene,
                        const TimelineLayout &layout) {
    int clipCount = TimelineClipCount(timeline);
    for (int i = 0; i < clipCount; ++i) {
        TimelineClipId id = kInvalidTimelineClipId;
        TimelineClip clip;
        if (!TimelineClipAt(timeline, i, &id, &clip)) {
            continue;
        }
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
                (double)(ui->mouseX - ui->timelineDragGrabX) / kTimelinePixelsPerSecond;
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
        Rect r = {TimelineTimeToX(layout.rulerX, startTime), laneY + 3.0f,
                  (float)(duration * kTimelinePixelsPerSecond), kTimelineLaneHeight - 6.0f};

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
                TimelineSubmitMoveClip(timeline, scene, id, layout.laneTracks[drawLane], startTime);
            } else if (ui->timelineDragMode == TimelineDrag_TrimRight) {
                double trimmed = ui->timelineDragDuration - duration;
                TimelineSubmitTrimClip(timeline, scene, id, ui->timelineDragTrimIn,
                                       ui->timelineDragTrimOut + trimmed, duration);
            } else {
                double shift = startTime - ui->timelineDragStartTime;
                TimelineSubmitTrimClip(timeline, scene, id, ui->timelineDragTrimIn + shift,
                                       ui->timelineDragTrimOut, duration);
            }
            ui->timelineDragClip = kInvalidTimelineClipId;
            ui->timelineDragMode = TimelineDrag_None;
        }
    }
}

void DrawTimelinePlayhead(UiState *ui, TimelineState *timeline, const TimelineLayout &layout) {
    float x = TimelineTimeToX(layout.rulerX, TimelineTime(timeline));
    if (x < layout.rulerX || x > layout.rulerRight) {
        return;
    }
    float bottom = layout.lanesY + layout.laneCount * kTimelineLaneHeight;
    PushRect(ui, {x, layout.ruler.y, 2.0f, bottom - layout.ruler.y}, theme::Playhead);
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
    TrackId track = lane >= 0 ? layout.laneTracks[lane] : kInvalidTrackId;
    double dropTime = TimelineXToTime(layout.rulerX, input.dropX);

    for (int i = 0; i < input.droppedFileCount; ++i) {
        ClipId wav = AudioLoadClip(ui->editor.audio, input.droppedFiles[i]);
        if (!ClipIdValid(wav)) {
            continue;
        }
        if (track == kInvalidTrackId) {
            track = TimelineSubmitAddTrack(ui->editor.timeline, ui->editor.scene,
                                           SelectedAudioSource(ui->editor.scene), "Track");
            if (track == kInvalidTrackId) {
                return;
            }
        }
        double duration = AudioClipDuration(ui->editor.audio, wav);
        TimelineSubmitCreateClip(ui->editor.timeline, ui->editor.scene, track, wav, dropTime,
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
    float lanesY = body.y + kTimelineTransportHeight + kRowGap + kTimelineRulerHeight;
    if (body.w < kTimelineHeaderWidth + 120.0f || body.h < kTimelineTransportHeight) {
        return;
    }

    TimelineLayout layout;
    layout.rulerX = body.x + kTimelineHeaderWidth;
    layout.rulerRight = body.x + body.w;
    layout.ruler = {layout.rulerX, body.y + kTimelineTransportHeight + kRowGap,
                    layout.rulerRight - layout.rulerX, kTimelineRulerHeight};
    layout.lanesY = lanesY;
    layout.bodyBottom = body.y + body.h;

    int visibleLanes = (int)((layout.bodyBottom - lanesY) / kTimelineLaneHeight);
    if (visibleLanes < 0) {
        visibleLanes = 0;
    }
    int trackCount = TimelineTrackCount(timeline);
    layout.laneCount = trackCount < visibleLanes ? trackCount : visibleLanes;
    for (int i = 0; i < layout.laneCount; ++i) {
        TimelineTrackAt(timeline, i, &layout.laneTracks[i], &layout.laneInfo[i]);
    }

    BuildTimelineTransport(ui, timeline, {body.x, body.y, body.w, kTimelineTransportHeight});
    BuildTimelineRuler(ui, timeline, layout);
    DrawTimelineLanes(ui, layout);
    BuildTimelineClips(ui, timeline, scene, layout);
    DrawTimelinePlayhead(ui, timeline, layout);
    HandleTimelineDrop(ui, input, layout);

    if (ui->timelineDragMode != TimelineDrag_None && !ui->mouseDown) {
        ui->timelineDragClip = kInvalidTimelineClipId; // the clip went away mid-drag
        ui->timelineDragMode = TimelineDrag_None;
    }
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
        bool on = ToolButtonOn(def, &ui->editor);
        if (ToolButtonWidget(ui, 700000 + i, r, def.label, on)) {
            if (def.kind == ToolBtn_Mode) {
                if (ui->editor.toolMode != nullptr) {
                    *ui->editor.toolMode = def.modeValue;
                }
            } else if (def.run != nullptr) {
                def.run(&ui->editor);
            }
        }
    }

    ui->toolbarRect = bar;
}

} // namespace

UiState *UiInit(Arena *arena, float drawableWidth, float drawableHeight) {
    UiState *ui = ArenaPushStruct(arena, UiState);
    ui->drawableWidth = drawableWidth;
    ui->drawableHeight = drawableHeight;
    ui->menuBar = MenuBarDefault();
    ui->openMenu = -1;
    ui->contentRect = {0.0f, 0.0f, drawableWidth, drawableHeight};
    return ui;
}

void UiHandleResize(UiState *ui, float drawableWidth, float drawableHeight) {
    ui->drawableWidth = drawableWidth;
    ui->drawableHeight = drawableHeight;
    for (int i = 0; i < kMaxPanels; ++i) {
        if (ui->panels[i].id != 0 && ui->panels[i].dock == UiDock_Float) {
            ui->panels[i].floatRect = ClampFloatRect(ui, ui->panels[i].floatRect);
        }
    }
}

void UiBuildFrame(UiState *ui, FrameInput input, CommandContext menuContext, UiDemoState *demo) {
    ui->vertexCount = 0;
    ui->menuContext = menuContext;
    ui->mouseX = input.mouseX;
    ui->mouseY = input.mouseY;
    ui->mouseDown = input.mouseLeftDown;
    ui->mousePressed = input.mouseLeftDown && !ui->prevMouseDown;
    ui->mouseReleased = !input.mouseLeftDown && ui->prevMouseDown;
    ui->hotId = 0;

    ui->menuBarVisible = input.fullscreen || menuContext.menuState->showMenuBar;
    ui->menuBarHeight = ui->menuBarVisible ? kMenuBarHeight : 0.0f;
    MenuUpdate(ui);
    if (ui->menuHasPointer) {
        ui->mousePressed = false;
    }

    ResolvePanelLayout(ui);
    UpdatePanelInteraction(ui);
    ResolvePanelLayout(ui); // reflect a drag/resize that started or ended this frame

    UiBeginPanel(ui, 1, "Controls", UiDock_Right, 264.0f);
    UiPanelButton(ui, "Button A");
    UiPanelButton(ui, "Button B");
    UiPanelButton(ui, "Reset");
    UiPanelSlider(ui, "Speed", &demo->speed);
    UiPanelSlider(ui, "Zoom", &demo->zoom);
    UiEndPanel(ui);

    UiBeginPanel(ui, 2, "Scene", UiDock_Left, 208.0f);
    char line[48];
    snprintf(line, sizeof(line), "View %d x %d", (int)ui->contentRect.w, (int)ui->contentRect.h);
    UiPanelText(ui, line);
    snprintf(line, sizeof(line), "Win  %d x %d", (int)ui->drawableWidth, (int)ui->drawableHeight);
    UiPanelText(ui, line);
    UiPanelButton(ui, "Reset Camera");
    UiEndPanel(ui);

    UiBeginPanel(ui, 3, "Timeline", UiDock_Bottom, 240.0f);
    BuildTimelineBody(ui, input);
    UiEndPanel(ui);

    if (ui->draggedPanel != 0) {
        PushBorder(ui, ui->dragOutline, 2.0f, kDragOutlineCol);
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
    PushText(ui, r.x + 8.0f, r.y + (kTitleBarHeight - kGlyphHeight * kTextScale) * 0.5f, title,
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
    ui->panelCursorY += kGlyphHeight * kTextScale + kRowGap;
}

CommandId UiTakeCommand(UiState *ui) {
    CommandId command = ui->pendingCommand;
    ui->pendingCommand = Command_None;
    return command;
}

void UiSetEditorState(UiState *ui, UiEditorState editor) {
    ui->editor = editor;
}

bool UiWantsMouse(const UiState *ui) {
    if (ui->menuHasPointer || ui->openMenu >= 0) {
        return true;
    }
    if (ui->draggedPanel != 0 || ui->resizePanel != 0 || ui->activeId != 0) {
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
    return ui->openMenu >= 0;
}

float UiContentOriginX(const UiState *ui) {
    return ui->contentRect.x;
}

float UiContentOriginY(const UiState *ui) {
    return ui->contentRect.y;
}

float UiContentWidth(const UiState *ui) {
    return ui->contentRect.w < 1.0f ? 1.0f : ui->contentRect.w;
}

float UiContentHeight(const UiState *ui) {
    return ui->contentRect.h < 1.0f ? 1.0f : ui->contentRect.h;
}

float UiDrawableWidth(const UiState *ui) {
    return ui->drawableWidth;
}

float UiDrawableHeight(const UiState *ui) {
    return ui->drawableHeight;
}

const UiVertex *UiVertices(const UiState *ui) {
    return ui->vertices;
}

int UiVertexCount(const UiState *ui) {
    return ui->vertexCount;
}
