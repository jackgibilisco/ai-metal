#include "menu.h"

namespace {

void RunQuit(CommandContext ctx) {
    ctx.hooks.quit(ctx.hooks.context);
}

void RunImportFile(CommandContext ctx) {
    ctx.hooks.importFile(ctx.hooks.context);
}

void RunToggleFullscreen(CommandContext ctx) {
    ctx.hooks.toggleFullscreen(ctx.hooks.context);
}

void RunToggleMenuBar(CommandContext ctx) {
    ctx.menuState->showMenuBar = !ctx.menuState->showMenuBar;
}

bool MenuBarNotLocked(CommandContext ctx) {
    return !ctx.fullscreen;
}

bool MenuBarShown(CommandContext ctx) {
    return ctx.menuState->showMenuBar;
}

const Command kCommands[] = {
    {Command_Quit, "Quit", {'q', ShortcutMod_Cmd}, nullptr, nullptr, RunQuit},
    {Command_ImportFile, "Import File...", {0, 0}, nullptr, nullptr, RunImportFile},
    {Command_ToggleFullscreen, "Toggle Full Screen", {'f', ShortcutMod_Cmd}, nullptr, nullptr,
     RunToggleFullscreen},
    {Command_ToggleMenuBar, "Toggle Menu Bar", {0, 0}, MenuBarNotLocked, MenuBarShown,
     RunToggleMenuBar},
};

} // namespace

const Command *CommandTable(int *count) {
    *count = (int)(sizeof(kCommands) / sizeof(kCommands[0]));
    return kCommands;
}

const Command *CommandById(CommandId id) {
    int count;
    const Command *table = CommandTable(&count);
    for (int i = 0; i < count; ++i) {
        if (table[i].id == id) {
            return &table[i];
        }
    }
    return nullptr;
}

const Command *CommandForShortcut(unsigned int key, unsigned int mods) {
    if (key == 0) {
        return nullptr;
    }
    int count;
    const Command *table = CommandTable(&count);
    for (int i = 0; i < count; ++i) {
        if (table[i].shortcut.key == key && table[i].shortcut.mods == mods) {
            return &table[i];
        }
    }
    return nullptr;
}

void CommandInvoke(CommandId id, CommandContext ctx) {
    const Command *command = CommandById(id);
    if (command == nullptr || command->invoke == nullptr) {
        return;
    }
    if (command->isEnabled != nullptr && !command->isEnabled(ctx)) {
        return;
    }
    command->invoke(ctx);
}

MenuBar MenuBarDefault() {
    MenuBar bar = {};
    bar.menuCount = 3;

    bar.menus[0].title = "Renderer";
    bar.menus[0].entryCount = 1;
    bar.menus[0].entries[0] = Command_Quit;

    bar.menus[1].title = "File";
    bar.menus[1].entryCount = 1;
    bar.menus[1].entries[0] = Command_ImportFile;

    bar.menus[2].title = "View";
    bar.menus[2].entryCount = 2;
    bar.menus[2].entries[0] = Command_ToggleFullscreen;
    bar.menus[2].entries[1] = Command_ToggleMenuBar;

    return bar;
}
