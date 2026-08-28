#include "ui.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr float kPanelStartWidth = 260.0f;
constexpr float kPanelMinWidth = 150.0f;
constexpr float kPanelMaxWidth = 520.0f;
constexpr float kMinContentWidth = 120.0f;
constexpr float kSplitterWidth = 6.0f;

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

constexpr int kMaxVertices = 65536;

struct Color {
    unsigned char r, g, b, a;
};

constexpr Color kPanelBg = {28, 30, 34, 255};
constexpr Color kSplitterCol = {58, 62, 70, 255};
constexpr Color kSplitterHot = {92, 142, 222, 255};
constexpr Color kButtonCol = {52, 57, 66, 255};
constexpr Color kButtonHot = {70, 77, 90, 255};
constexpr Color kButtonActive = {92, 142, 222, 255};
constexpr Color kTrackCol = {42, 46, 53, 255};
constexpr Color kHandleCol = {126, 174, 236, 255};
constexpr Color kHandleHot = {170, 202, 244, 255};
constexpr Color kTextCol = {224, 227, 232, 255};
constexpr Color kTextDisabled = {120, 124, 130, 255};
constexpr Color kTextShortcut = {148, 152, 158, 255};
constexpr Color kMenuBarBg = {22, 24, 27, 255};
constexpr Color kMenuTitleHot = {70, 77, 90, 255};
constexpr Color kMenuTitleOpen = {92, 142, 222, 255};
constexpr Color kMenuDropdownBg = {38, 41, 46, 255};
constexpr Color kMenuItemHot = {70, 77, 90, 255};

struct Rect {
    float x, y, w, h;
};

bool PointInRect(float px, float py, Rect r) {
    return px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h;
}

} // namespace

struct UiState {
    float drawableWidth;
    float drawableHeight;
    float panelWidth;
    float contentWidth;
    bool draggingSplitter;

    MenuBar menuBar;
    CommandContext menuContext;    // set each frame; menu rendering queries it
    int openMenu;                  // -1 when closed
    CommandId pendingCommand;
    bool menuBarVisible;
    float menuBarHeight;           // 0 when the strip is hidden
    bool menuHasPointer;           // the strip/dropdown owns the cursor this frame

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
        if (t < 0.0f) {
            t = 0.0f;
        }
        if (t > 1.0f) {
            t = 1.0f;
        }
        *value = t;
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
                     kMenuTitleHot);
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

} // namespace

UiState *UiInit(Arena *arena, float drawableWidth, float drawableHeight) {
    UiState *ui = ArenaPushStruct(arena, UiState);
    ui->drawableWidth = drawableWidth;
    ui->drawableHeight = drawableHeight;
    ui->panelWidth = kPanelStartWidth;
    ui->contentWidth = drawableWidth - kPanelStartWidth - kSplitterWidth;
    ui->menuBar = MenuBarDefault();
    ui->openMenu = -1;
    return ui;
}

void UiHandleResize(UiState *ui, float drawableWidth, float drawableHeight) {
    ui->drawableWidth = drawableWidth;
    ui->drawableHeight = drawableHeight;
    float maxWidth = drawableWidth - kMinContentWidth - kSplitterWidth;
    if (maxWidth > kPanelMaxWidth) {
        maxWidth = kPanelMaxWidth;
    }
    if (ui->panelWidth > maxWidth) {
        ui->panelWidth = maxWidth;
    }
    if (ui->panelWidth < kPanelMinWidth) {
        ui->panelWidth = kPanelMinWidth;
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

    float splitterHitX = ui->drawableWidth - ui->panelWidth - kSplitterWidth;
    Rect splitterHit = {splitterHitX, 0.0f, kSplitterWidth, ui->drawableHeight};
    bool splitterHover = PointInRect(ui->mouseX, ui->mouseY, splitterHit);

    if (splitterHover && ui->mousePressed) {
        ui->draggingSplitter = true;
    }
    if (!ui->mouseDown) {
        ui->draggingSplitter = false;
    }
    if (ui->draggingSplitter) {
        float maxWidth = ui->drawableWidth - kMinContentWidth - kSplitterWidth;
        if (maxWidth > kPanelMaxWidth) {
            maxWidth = kPanelMaxWidth;
        }
        ui->panelWidth = ui->drawableWidth - ui->mouseX - kSplitterWidth * 0.5f;
        if (ui->panelWidth < kPanelMinWidth) {
            ui->panelWidth = kPanelMinWidth;
        }
        if (ui->panelWidth > maxWidth) {
            ui->panelWidth = maxWidth;
        }
    }

    float contentWidth = floorf(ui->drawableWidth - ui->panelWidth - kSplitterWidth);
    if (contentWidth < 1.0f) {
        contentWidth = 1.0f;
    }
    ui->contentWidth = contentWidth;

    float splitterX = contentWidth;
    float panelX = contentWidth + kSplitterWidth;
    float panelW = ui->drawableWidth - panelX;

    PushRect(ui, {panelX, 0.0f, panelW, ui->drawableHeight}, kPanelBg);
    PushRect(ui, {splitterX, 0.0f, kSplitterWidth, ui->drawableHeight},
             (splitterHover || ui->draggingSplitter) ? kSplitterHot : kSplitterCol);

    float widgetX = panelX + kPadding;
    float widgetW = panelW - kPadding * 2.0f;
    float cursorY = ui->menuBarHeight + kPadding;

    Button(ui, 1, {widgetX, cursorY, widgetW, kButtonHeight}, "Button A");
    cursorY += kButtonHeight + kRowGap;
    Button(ui, 2, {widgetX, cursorY, widgetW, kButtonHeight}, "Button B");
    cursorY += kButtonHeight + kRowGap;
    Button(ui, 3, {widgetX, cursorY, widgetW, kButtonHeight}, "Reset");
    cursorY += kButtonHeight + kRowGap * 2.0f;

    Slider(ui, 10, {widgetX, cursorY, widgetW, kSliderHeight}, "Speed", &demo->speed);
    cursorY += kSliderHeight + kRowGap;
    Slider(ui, 11, {widgetX, cursorY, widgetW, kSliderHeight}, "Zoom", &demo->zoom);

    MenuDraw(ui);

    if (ui->mouseReleased) {
        ui->activeId = 0;
    }
    ui->prevMouseDown = ui->mouseDown;
}

CommandId UiTakeCommand(UiState *ui) {
    CommandId command = ui->pendingCommand;
    ui->pendingCommand = Command_None;
    return command;
}

bool UiWantsMouse(const UiState *ui) {
    bool overPanel = ui->mouseX >= ui->contentWidth;
    bool dragging = ui->draggingSplitter || ui->activeId != 0;
    return overPanel || ui->menuHasPointer || dragging || ui->openMenu >= 0;
}

bool UiWantsKeyboard(const UiState *ui) {
    return ui->openMenu >= 0;
}

float UiContentOriginX(const UiState *ui) {
    (void)ui;
    return 0.0f;
}

float UiContentOriginY(const UiState *ui) {
    return ui->menuBarHeight;
}

float UiContentWidth(const UiState *ui) {
    return ui->contentWidth < 1.0f ? 1.0f : ui->contentWidth;
}

float UiContentHeight(const UiState *ui) {
    float height = ui->drawableHeight - ui->menuBarHeight;
    return height < 1.0f ? 1.0f : height;
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
