#pragma once

// The application menu as portable data. Both the native macOS menu bar and
// the in-app menu strip are built from this one model, and both route clicks
// through MenuInvoke, so a second platform only has to render the strip and
// fill in PlatformMenuHooks.

enum MenuAction {
    MenuAction_None = 0,
    MenuAction_ImportFile,
    MenuAction_ToggleFullscreen,
    MenuAction_ToggleMenuBar,
    MenuAction_Quit,
};

struct MenuItem {
    const char *label;
    MenuAction action;
    const char *shortcut; // one lowercase key, meaning Cmd-<key>; "" for none
};

constexpr int kMaxMenuItems = 8;

struct Menu {
    const char *title;
    MenuItem items[kMaxMenuItems];
    int itemCount;
};

constexpr int kMaxMenus = 6;

struct MenuBar {
    Menu menus[kMaxMenus];
    int menuCount;
};

// State a menu action mutates directly, rather than handing off to the
// platform. The strip is force-shown while fullscreen regardless of this.
struct MenuState {
    bool showMenuBar;
};

// Filled in by the platform layer so portable code can trigger the
// platform-only actions without knowing about AppKit.
struct PlatformMenuHooks {
    void (*importFile)(void *context);
    void (*toggleFullscreen)(void *context);
    void (*quit)(void *context);
    void *context;
};

MenuBar MenuBarDefault();
void MenuInvoke(MenuAction action, MenuState *state, PlatformMenuHooks hooks);
