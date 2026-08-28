#include "ui.h"

#include <cmath>
#include <cstdio>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "third_party/stb_easy_font.h"
#pragma clang diagnostic pop

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
constexpr float kGlyphHeight = 7.0f;

constexpr float kMenuBarHeight = 32.0f;
constexpr float kMenuTitlePadX = 12.0f;
constexpr float kMenuRowHeight = 26.0f;
constexpr float kMenuDropdownPadX = 14.0f;
constexpr float kMenuDropdownMinWidth = 170.0f;
constexpr float kMenuShortcutColumn = 64.0f;

constexpr int kMaxVertices = 16384;

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
    int openMenu;                  // -1 when closed
    MenuAction pendingMenuAction;
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

    float sliderValue[2];

    UiVertex vertices[kMaxVertices];
    int vertexCount;
};

namespace {

void PushVertex(UiState *ui, float x, float y, Color c) {
    if (ui->vertexCount >= kMaxVertices) {
        return;
    }
    UiVertex &v = ui->vertices[ui->vertexCount++];
    v.x = x;
    v.y = y;
    v.rgba[0] = c.r;
    v.rgba[1] = c.g;
    v.rgba[2] = c.b;
    v.rgba[3] = c.a;
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

void PushText(UiState *ui, float x, float y, const char *text, Color c) {
    static char quadBuffer[16384];
    unsigned char color[4] = {c.r, c.g, c.b, c.a};
    int quadCount =
        stb_easy_font_print(0.0f, 0.0f, (char *)text, color, quadBuffer, sizeof(quadBuffer));

    const char *cursor = quadBuffer;
    for (int q = 0; q < quadCount; ++q) {
        float px[4];
        float py[4];
        for (int i = 0; i < 4; ++i) {
            float vx = *(const float *)(cursor + 0);
            float vy = *(const float *)(cursor + 4);
            px[i] = x + vx * kTextScale;
            py[i] = y + vy * kTextScale;
            cursor += 16;
        }
        PushQuad(ui, px[0], py[0], px[1], py[1], px[2], py[2], px[3], py[3], c);
    }
}

float TextWidth(const char *text) {
    return (float)stb_easy_font_width((char *)text) * kTextScale;
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

Rect MenuDropdownRect(const UiState *ui, int menuIndex) {
    const Menu &menu = ui->menuBar.menus[menuIndex];
    float width = kMenuDropdownMinWidth;
    for (int i = 0; i < menu.itemCount; ++i) {
        float w = TextWidth(menu.items[i].label) + kMenuDropdownPadX * 2.0f;
        if (menu.items[i].shortcut && menu.items[i].shortcut[0] != '\0') {
            w += kMenuShortcutColumn;
        }
        if (w > width) {
            width = w;
        }
    }
    return {MenuTitleX(ui, menuIndex), kMenuBarHeight, width,
            (float)menu.itemCount * kMenuRowHeight};
}

bool MenuItemLocked(MenuAction action, bool fullscreen) {
    return action == MenuAction_ToggleMenuBar && fullscreen;
}

void MenuUpdate(UiState *ui, bool fullscreen) {
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
            if (ui->mousePressed && row >= 0 && row < menu.itemCount &&
                !MenuItemLocked(menu.items[row].action, fullscreen)) {
                ui->pendingMenuAction = menu.items[row].action;
                ui->openMenu = -1;
            }
        } else if (ui->mousePressed) {
            ui->openMenu = -1;
        }
    }

    if (pointerInBar) {
        ui->menuHasPointer = true;
    }
}

void MenuDraw(UiState *ui, bool fullscreen) {
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

    int menuIndex = ui->openMenu;
    const Menu &menu = ui->menuBar.menus[menuIndex];
    Rect dropdown = MenuDropdownRect(ui, menuIndex);
    PushRect(ui, dropdown, kMenuDropdownBg);

    for (int i = 0; i < menu.itemCount; ++i) {
        MenuItem item = menu.items[i];
        Rect row = {dropdown.x, dropdown.y + (float)i * kMenuRowHeight, dropdown.w, kMenuRowHeight};
        bool locked = MenuItemLocked(item.action, fullscreen);
        if (!locked && PointInRect(ui->mouseX, ui->mouseY, row)) {
            PushRect(ui, row, kMenuItemHot);
        }
        float textY = row.y + (kMenuRowHeight - kGlyphHeight * kTextScale) * 0.5f;
        PushText(ui, row.x + kMenuDropdownPadX, textY, item.label, locked ? kTextDisabled : kTextCol);
        if (item.shortcut && item.shortcut[0] != '\0') {
            char combo[8];
            snprintf(combo, sizeof(combo), "Cmd+%c", (char)(item.shortcut[0] - 32));
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
    ui->sliderValue[0] = 0.5f;
    ui->sliderValue[1] = 0.35f;
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

void UiBuildFrame(UiState *ui, FrameInput input, bool showMenuBarPref) {
    ui->vertexCount = 0;
    ui->mouseX = input.mouseX;
    ui->mouseY = input.mouseY;
    ui->mouseDown = input.mouseDown;
    ui->mousePressed = input.mouseDown && !ui->prevMouseDown;
    ui->mouseReleased = !input.mouseDown && ui->prevMouseDown;
    ui->hotId = 0;

    ui->menuBarVisible = input.fullscreen || showMenuBarPref;
    ui->menuBarHeight = ui->menuBarVisible ? kMenuBarHeight : 0.0f;
    MenuUpdate(ui, input.fullscreen);
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

    Slider(ui, 10, {widgetX, cursorY, widgetW, kSliderHeight}, "Speed", &ui->sliderValue[0]);
    cursorY += kSliderHeight + kRowGap;
    Slider(ui, 11, {widgetX, cursorY, widgetW, kSliderHeight}, "Zoom", &ui->sliderValue[1]);

    MenuDraw(ui, input.fullscreen);

    if (ui->mouseReleased) {
        ui->activeId = 0;
    }
    ui->prevMouseDown = ui->mouseDown;
}

MenuAction UiTakeMenuAction(UiState *ui) {
    MenuAction action = ui->pendingMenuAction;
    ui->pendingMenuAction = MenuAction_None;
    return action;
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
