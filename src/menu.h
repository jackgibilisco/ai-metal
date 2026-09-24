#pragma once

// The application's commands as portable data. One command table is the single
// source of truth for every action, its label, its enabled/checked state, and
// its keyboard shortcut. The native macOS menu bar, the in-app menu strip, and
// the keybinding matcher all resolve through it, so a platform with no native
// menu (a Windows port) still gets working shortcuts by scanning key events.

#include "editor_flags.h"

typedef int CommandId;

enum {
    Command_None = 0,
    Command_Quit,
    Command_ImportFile,
    Command_ToggleFullscreen,
    Command_ToggleMenuBar,
    Command_ToggleFrameStats,
    Command_CycleAoDebug,
    Command_ToggleFxaa,
    Command_ToggleLayoutBounds,
    Command_ResetPanels,
};

// F3 is a held prefix, not a real modifier key: the app ORs this bit into every
// key event while F3 is down, so "F3+o" matches like any other shortcut. F3
// tapped alone (no chord) is its own shortcut, key 0.
enum {
    ShortcutMod_Cmd = 1 << 0,
    ShortcutMod_Shift = 1 << 1,
    ShortcutMod_Ctrl = 1 << 2,
    ShortcutMod_Alt = 1 << 3,
    ShortcutMod_F3 = 1 << 4,
};

// key is a lowercase ASCII codepoint; 0 with no mods means the command has no
// shortcut.
struct Shortcut {
    unsigned int key;
    unsigned int mods;
};

// State a command mutates directly rather than handing off to the platform.
// The menu strip is force-shown while fullscreen regardless of showMenuBar.
struct MenuState {
    bool showMenuBar;
};

// Filled in by the platform layer so portable code can trigger the
// platform-only commands without knowing about AppKit or Win32.
struct PlatformMenuHooks {
    // Shows a native open-file dialog filtered to `extension` (no dot). The
    // pick, if any, arrives later as FrameInput.openedFile.
    void (*showOpenDialog)(void *context, const char *extension);
    void (*toggleFullscreen)(void *context);
    void (*quit)(void *context);
    void *context;
};

// Everything a command's predicates and action need. Query-only code (menu
// rendering, validation) reads menuState/fullscreen and never touches hooks.
struct CommandContext {
    MenuState *menuState;
    EditorFlags *flags;
    PlatformMenuHooks hooks;
    bool fullscreen;
};

struct Command {
    CommandId id;
    const char *label;
    Shortcut shortcut;
    bool (*isEnabled)(CommandContext ctx); // null: always enabled
    bool (*isChecked)(CommandContext ctx); // null: not a checkable item
    void (*invoke)(CommandContext ctx);
};

const Command *CommandTable(int *count);
const Command *CommandById(CommandId id);
const Command *CommandForShortcut(unsigned int key, unsigned int mods);
void CommandInvoke(CommandId id, CommandContext ctx);

// What a native menu item needs to draw itself: greyed out, and whether it
// shows a check mark.
struct CommandState {
    bool enabled;
    bool checkable;
    bool checked;
};

CommandState CommandQueryState(const Command *command, CommandContext ctx);

// Menu layout: which commands appear under which title, in order. Entries are
// command ids; kMenuSeparator draws a divider.
constexpr CommandId kMenuSeparator = -1;
constexpr int kMaxMenuEntries = 8;

struct Menu {
    const char *title;
    CommandId entries[kMaxMenuEntries];
    int entryCount;
};

constexpr int kMaxMenus = 8;

struct MenuBar {
    Menu menus[kMaxMenus];
    int menuCount;
};

MenuBar MenuBarDefault();
