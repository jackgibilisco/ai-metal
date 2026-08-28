#include "menu.h"

MenuBar MenuBarDefault() {
    MenuBar bar = {};
    bar.menuCount = 3;

    bar.menus[0].title = "Renderer";
    bar.menus[0].itemCount = 1;
    bar.menus[0].items[0] = {"Quit", MenuAction_Quit, "q"};

    bar.menus[1].title = "File";
    bar.menus[1].itemCount = 1;
    bar.menus[1].items[0] = {"Import File...", MenuAction_ImportFile, ""};

    bar.menus[2].title = "View";
    bar.menus[2].itemCount = 2;
    bar.menus[2].items[0] = {"Toggle Full Screen", MenuAction_ToggleFullscreen, "f"};
    bar.menus[2].items[1] = {"Toggle Menu Bar", MenuAction_ToggleMenuBar, ""};

    return bar;
}

void MenuInvoke(MenuAction action, MenuState *state, PlatformMenuHooks hooks) {
    switch (action) {
        case MenuAction_ImportFile:
            hooks.importFile(hooks.context);
            break;
        case MenuAction_ToggleFullscreen:
            hooks.toggleFullscreen(hooks.context);
            break;
        case MenuAction_Quit:
            hooks.quit(hooks.context);
            break;
        case MenuAction_ToggleMenuBar:
            state->showMenuBar = !state->showMenuBar;
            break;
        case MenuAction_None:
            break;
    }
}
