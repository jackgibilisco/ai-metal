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

void RunToggleFrameStats(CommandContext ctx) {
    ctx.flags->frameStatsHud = !ctx.flags->frameStatsHud;
}

void RunCycleAoDebug(CommandContext ctx) {
    ctx.flags->aoDebugView = (ctx.flags->aoDebugView + 1) % 3;
}

void RunToggleFxaa(CommandContext ctx) {
    ctx.flags->fxaaEnabled = !ctx.flags->fxaaEnabled;
}

void RunToggleLayoutBounds(CommandContext ctx) {
    ctx.flags->layoutBounds = !ctx.flags->layoutBounds;
}

void RunResetPanels(CommandContext ctx) {
    ctx.flags->resetPanelsRequested = true;
}

bool FrameStatsShown(CommandContext ctx) { return ctx.flags->frameStatsHud; }
bool AoDebugOn(CommandContext ctx) { return ctx.flags->aoDebugView != 0; }
bool FxaaOn(CommandContext ctx) { return ctx.flags->fxaaEnabled; }
bool LayoutBoundsShown(CommandContext ctx) { return ctx.flags->layoutBounds; }

const Command kCommands[] = {
    {Command_Quit, "Quit", {'q', ShortcutMod_Cmd}, nullptr, nullptr, RunQuit},
    {Command_ImportFile, "Import File...", {0, 0}, nullptr, nullptr, RunImportFile},
    {Command_ToggleFullscreen, "Toggle Full Screen", {'f', ShortcutMod_Cmd}, nullptr, nullptr,
     RunToggleFullscreen},
    {Command_ToggleMenuBar, "Toggle Menu Bar", {0, 0}, MenuBarNotLocked, MenuBarShown,
     RunToggleMenuBar},
    {Command_ToggleFrameStats, "Frame Timing HUD", {0, ShortcutMod_F3}, nullptr, FrameStatsShown,
     RunToggleFrameStats},
    {Command_CycleAoDebug, "Cycle AO Debug View", {'o', ShortcutMod_F3}, nullptr, AoDebugOn,
     RunCycleAoDebug},
    {Command_ToggleFxaa, "FXAA", {'f', ShortcutMod_F3}, nullptr, FxaaOn, RunToggleFxaa},
    {Command_ToggleLayoutBounds, "Layout Bounds", {'b', ShortcutMod_F3}, nullptr,
     LayoutBoundsShown, RunToggleLayoutBounds},
    {Command_ResetPanels, "Reset Panel Layout", {0, 0}, nullptr, nullptr, RunResetPanels},
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
    bar.menuCount = 5;

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

    bar.menus[3].title = "Debug";
    bar.menus[3].entryCount = 4;
    bar.menus[3].entries[0] = Command_ToggleFrameStats;
    bar.menus[3].entries[1] = Command_CycleAoDebug;
    bar.menus[3].entries[2] = Command_ToggleFxaa;
    bar.menus[3].entries[3] = Command_ToggleLayoutBounds;

    bar.menus[4].title = "Window";
    bar.menus[4].entryCount = 1;
    bar.menus[4].entries[0] = Command_ResetPanels;

    return bar;
}
