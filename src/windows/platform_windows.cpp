// The Windows platform layer: window, raw input, native menu bar, borderless
// fullscreen, drag and drop, the open-file dialog, and the demand-driven frame
// loop. It reports what happened and leaves every decision about what it means
// (shortcuts, camera drags, which files to accept, the frame-timing HUD) to
// app.cpp. It names no graphics API: platform_windows_gl.cpp owns the context
// and the frame target behind platform_windows_present.h.

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>

#include "app.h"
#include "arena.h"
#include "frame_input.h"
#include "menu.h"
#include "platform_windows_present.h"

namespace {

constexpr size_t kArenaSize = 192 * 1024 * 1024;
constexpr wchar_t kWindowClassName[] = L"RendererWindow";

// A mouse wheel reports one coarse notch at a time; scale it into the same
// range a trackpad pinch produces, as the macOS layer does.
constexpr float kMouseWheelZoom = 0.05f;
constexpr float kPinchPerNotch = 0.05f;

constexpr int kMaxDroppedFiles = 16;
constexpr int kMaxPathLength = 1024;

// Menu item ids carry the CommandId, offset clear of the system SC_* range.
constexpr UINT kMenuIdBase = 0x1000;

// A drag from another app leaves this window inactive, which normally pauses
// the loop. Frames keep running this long after the last drag event so the
// drop ghost tracks the cursor and the drop itself (or its removal) repaints.
constexpr int kDragFrames = 4;

struct PlatformState {
    Arena arena;
    HWND window;
    Presenter *presenter;
    HMENU menuBar;

    bool ready; // false until the presenter and the app exist
    bool running;
    bool active;
    bool minimized;
    bool fullscreen;
    WINDOWPLACEMENT windowedPlacement;
    LONG windowedStyle;

    FrameInput pending;
    int lastMouseX;
    int lastMouseY;
    char droppedPaths[kMaxDroppedFiles][kMaxPathLength];
    const char *droppedPathPointers[kMaxDroppedFiles];
    char openedPath[kMaxPathLength];

    LARGE_INTEGER counterFrequency;
    LARGE_INTEGER lastCounter; // zeroed to restart the clock after a pause

    int dragFramesLeft; // frames to run despite being inactive; see kDragFrames
};

PlatformState g_platform;

// ---------------------------------------------------------------- key events

// Windows virtual key codes for the keys app.cpp and ui.cpp act on. They stay
// in this file; FrameInput carries the portable Key_* value instead.
int NormalizedKeyCode(WPARAM virtualKey) {
    switch (virtualKey) {
    case VK_RETURN: return Key_Return;
    case VK_ESCAPE: return Key_Escape;
    case VK_BACK: return Key_Backspace;
    case VK_DELETE: return Key_Delete;
    case VK_LEFT: return Key_Left;
    case VK_RIGHT: return Key_Right;
    case VK_F3: return Key_F3;
    default: return Key_None;
    }
}

bool KeyDown(int virtualKey) { return GetKeyState(virtualKey) < 0; }

unsigned int ModifierBits() {
    unsigned int mods = 0;
    // There is no Cmd key here. Ctrl stands in for it, so every
    // ShortcutMod_Cmd binding in the command table fires on Ctrl.
    if (KeyDown(VK_CONTROL)) mods |= ShortcutMod_Cmd | ShortcutMod_Ctrl;
    if (KeyDown(VK_SHIFT)) mods |= ShortcutMod_Shift;
    if (KeyDown(VK_MENU)) mods |= ShortcutMod_Alt;
    return mods;
}

void EnqueueKey(HWND window, WPARAM virtualKey, bool pressed) {
    if (g_platform.pending.keyEventCount >= kMaxKeyEvents) {
        return;
    }
    // TranslateMessage has already queued this keystroke's WM_CHAR, so taking
    // it here gives the shifted character a text field wants. Keys that
    // produce none (and every key-up) fall back to the unshifted layout
    // character, which is what the shortcut table matches on. With Ctrl held,
    // WM_CHAR instead holds a control code (Ctrl+Z is 0x1A, not 'z'), so that
    // case also falls back to the unshifted character.
    unsigned int codepoint = 0;
    MSG character;
    if (pressed && !KeyDown(VK_CONTROL) &&
        PeekMessageW(&character, window, WM_CHAR, WM_CHAR, PM_REMOVE)) {
        codepoint = (unsigned int)character.wParam;
    } else {
        codepoint = MapVirtualKeyW((UINT)virtualKey, MAPVK_VK_TO_CHAR) & 0x7FFFu;
    }
    int keyCode = NormalizedKeyCode(virtualKey);
    if (keyCode != Key_None) {
        codepoint = 0;
    }
    KeyEvent event = {keyCode, codepoint, ModifierBits(), pressed};
    g_platform.pending.keyEvents[g_platform.pending.keyEventCount++] = event;
}

// ---------------------------------------------------------------- menu bar

void Utf8ToWide(const char *utf8, wchar_t *out, int outCount) {
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outCount);
}

void WideToUtf8(const wchar_t *wide, char *out, int outCount) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outCount, nullptr, nullptr);
}

HMENU BuildMenuBar() {
    MenuBar layout = MenuBarDefault();
    HMENU bar = CreateMenu();
    for (int i = 0; i < layout.menuCount; ++i) {
        HMENU popup = CreatePopupMenu();
        for (int j = 0; j < layout.menus[i].entryCount; ++j) {
            CommandId entry = layout.menus[i].entries[j];
            if (entry == kMenuSeparator) {
                AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            wchar_t label[128];
            Utf8ToWide(CommandById(entry)->label, label, 128);
            AppendMenuW(popup, MF_STRING, kMenuIdBase + (UINT)entry, label);
        }
        wchar_t title[64];
        Utf8ToWide(layout.menus[i].title, title, 64);
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)popup, title);
    }
    return bar;
}

// The analogue of macOS's -validateMenuItem:, run just before a popup opens.
void UpdateMenuPopup(HMENU popup) {
    int count = GetMenuItemCount(popup);
    for (int i = 0; i < count; ++i) {
        UINT itemId = GetMenuItemID(popup, i);
        if (itemId == 0 || itemId == (UINT)-1) {
            continue; // a separator or a submenu
        }
        CommandState state = AppCommandState(&g_platform.arena, (CommandId)(itemId - kMenuIdBase));
        if (state.checkable) {
            CheckMenuItem(popup, itemId, MF_BYCOMMAND | (state.checked ? MF_CHECKED : MF_UNCHECKED));
        }
        EnableMenuItem(popup, itemId, MF_BYCOMMAND | (state.enabled ? MF_ENABLED : MF_GRAYED));
    }
}

// ---------------------------------------------------------------- window size

int MonitorRefreshHz(HWND window) {
    MONITORINFOEXW monitor = {};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) ||
        mode.dmDisplayFrequency <= 1) {
        return 60;
    }
    return (int)mode.dmDisplayFrequency;
}

void ResizeToClient() {
    RECT client;
    GetClientRect(g_platform.window, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width < 1 || height < 1) {
        return;
    }
    PresenterResize(g_platform.presenter, width, height);
    FrameResize(&g_platform.arena, (float)width, (float)height);
}

void ToggleBorderlessFullscreen() {
    HWND window = g_platform.window;
    g_platform.fullscreen = !g_platform.fullscreen;
    if (g_platform.fullscreen) {
        g_platform.windowedPlacement.length = sizeof(g_platform.windowedPlacement);
        GetWindowPlacement(window, &g_platform.windowedPlacement);
        g_platform.windowedStyle = GetWindowLongW(window, GWL_STYLE);

        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT screen = monitor.rcMonitor;

        SetMenu(window, nullptr); // the in-app strip takes over
        SetWindowLongW(window, GWL_STYLE, g_platform.windowedStyle & ~(LONG)WS_OVERLAPPEDWINDOW);
        // Unlike macOS, covering the monitor exactly is the right thing here:
        // the 1px overhang there dodges a WindowServer direct-scanout path that
        // DWM has no equivalent of.
        SetWindowPos(window, HWND_TOP, screen.left, screen.top, screen.right - screen.left,
                     screen.bottom - screen.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(window, GWL_STYLE, g_platform.windowedStyle);
        SetMenu(window, g_platform.menuBar);
        SetWindowPlacement(window, &g_platform.windowedPlacement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    ResizeToClient();
}

// ------------------------------------------------------------- menu hooks

void MenuHookShowOpenDialog(void *, const char *extension) {
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    wchar_t pattern[32];
    swprintf(pattern, 32, L"*.%hs", extension);
    COMDLG_FILTERSPEC filter = {pattern, pattern};
    dialog->SetFileTypes(1, &filter);
    dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->Show(g_platform.window)) && SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR widePath = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath))) {
            WideToUtf8(widePath, g_platform.openedPath, kMaxPathLength);
            g_platform.pending.openedFile = g_platform.openedPath;
            CoTaskMemFree(widePath);
        }
        item->Release();
    }
    dialog->Release();

    // The dialog ran its own message pump, so the frame clock is stale.
    g_platform.lastCounter.QuadPart = 0;
}

void MenuHookToggleFullscreen(void *) { ToggleBorderlessFullscreen(); }

void MenuHookQuit(void *) { g_platform.running = false; }

// ------------------------------------------------------------- drag and drop

// Returns how many dropped files the app accepts, and when `store` is true
// copies their paths into the platform's buffers for this frame's FrameInput.
int CollectAcceptedFiles(IDataObject *dataObject, bool store) {
    FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    if (FAILED(dataObject->GetData(&format, &medium))) {
        return 0;
    }
    HDROP drop = (HDROP)GlobalLock(medium.hGlobal);
    UINT fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

    int acceptedCount = 0;
    for (UINT i = 0; i < fileCount && acceptedCount < kMaxDroppedFiles; ++i) {
        wchar_t widePath[kMaxPathLength];
        DragQueryFileW(drop, i, widePath, kMaxPathLength);
        char scratch[kMaxPathLength];
        char *path = store ? g_platform.droppedPaths[acceptedCount] : scratch;
        WideToUtf8(widePath, path, kMaxPathLength);
        if (AppAcceptsDroppedFile(path)) {
            ++acceptedCount; // otherwise the next file reuses this slot
        }
    }
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return acceptedCount;
}

POINT ScreenToClientPoint(POINTL screenPoint) {
    POINT client = {screenPoint.x, screenPoint.y};
    ScreenToClient(g_platform.window, &client);
    return client;
}

void SetDragHover(POINTL screenPoint, int fileCount) {
    POINT client = ScreenToClientPoint(screenPoint);
    g_platform.pending.dragHovering = fileCount > 0;
    g_platform.pending.dragHoverX = (float)client.x;
    g_platform.pending.dragHoverY = (float)client.y;
    g_platform.pending.dragHoverFileCount = fileCount;
    g_platform.dragFramesLeft = kDragFrames;
}

// COM mandates the vtable; this is the one place polymorphism is the API.
struct FileDropTarget : IDropTarget {
    int hoveredFileCount = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *dataObject, DWORD, POINTL point,
                                        DWORD *effect) override {
        hoveredFileCount = CollectAcceptedFiles(dataObject, false);
        return DragOver(0, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD *effect) override {
        SetDragHover(point, hoveredFileCount);
        *effect = hoveredFileCount > 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        hoveredFileCount = 0;
        g_platform.pending.dragHovering = false;
        g_platform.pending.dragHoverFileCount = 0;
        g_platform.dragFramesLeft = kDragFrames;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *dataObject, DWORD, POINTL point,
                                   DWORD *effect) override {
        int count = CollectAcceptedFiles(dataObject, true);
        POINT client = ScreenToClientPoint(point);
        g_platform.pending.droppedFiles = g_platform.droppedPathPointers;
        g_platform.pending.droppedFileCount = count;
        g_platform.pending.dropX = (float)client.x;
        g_platform.pending.dropY = (float)client.y;
        DragLeave();
        *effect = count > 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
};

FileDropTarget g_dropTarget;

// -------------------------------------------------------------- window proc

void TrackMouse(LPARAM lParam) {
    g_platform.pending.mouseX = (float)GET_X_LPARAM(lParam);
    g_platform.pending.mouseY = (float)GET_Y_LPARAM(lParam);
}

void SetMouseButton(bool *button, bool down, LPARAM lParam) {
    TrackMouse(lParam);
    *button = down;
    const FrameInput &pending = g_platform.pending;
    bool anyDown = pending.mouseLeftDown || pending.mouseRightDown || pending.mouseMiddleDown;
    // Capture keeps drags alive when the pointer leaves the window.
    if (anyDown && GetCapture() != g_platform.window) {
        SetCapture(g_platform.window);
    } else if (!anyDown && GetCapture() == g_platform.window) {
        ReleaseCapture();
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    FrameInput &pending = g_platform.pending;
    switch (message) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_platform.running = false;
        return 0;

    case WM_ACTIVATEAPP:
        g_platform.active = wParam != FALSE;
        return 0;

    case WM_SIZE:
        g_platform.minimized = wParam == SIZE_MINIMIZED;
        if (!g_platform.minimized && g_platform.ready) {
            ResizeToClient();
        }
        return 0;

    case WM_MOVE:
    case WM_DISPLAYCHANGE:
        pending.displayRefreshHz = (float)MonitorRefreshHz(window);
        return 0;

    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lParam;
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_INITMENUPOPUP:
        UpdateMenuPopup((HMENU)wParam);
        return 0;

    case WM_COMMAND:
        AppInvokeCommand(&g_platform.arena, (CommandId)(LOWORD(wParam) - kMenuIdBase));
        return 0;

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        float scale = (float)GetDpiForWindow(window) / 96.0f;
        pending.mouseDeltaX += (float)(x - g_platform.lastMouseX) / scale;
        pending.mouseDeltaY += (float)(y - g_platform.lastMouseY) / scale;
        g_platform.lastMouseX = x;
        g_platform.lastMouseY = y;
        TrackMouse(lParam);
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        SetMouseButton(&pending.mouseLeftDown, message == WM_LBUTTONDOWN, lParam);
        return 0;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        SetMouseButton(&pending.mouseRightDown, message == WM_RBUTTONDOWN, lParam);
        return 0;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        SetMouseButton(&pending.mouseMiddleDown, message == WM_MBUTTONDOWN, lParam);
        return 0;

    case WM_MOUSEWHEEL: {
        float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        // A precision touchpad sends a pinch as ctrl+wheel, which is also what
        // every Windows app treats as zoom, so that is where magnification
        // comes from here.
        if (KeyDown(VK_CONTROL)) {
            pending.magnification += notches * kPinchPerNotch;
            pending.zoomDelta += notches * kPinchPerNotch;
        } else {
            pending.scrollY += notches;
            pending.zoomDelta += notches * kMouseWheelZoom;
        }
        return 0;
    }

    case WM_MOUSEHWHEEL:
        pending.scrollX += (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((lParam & (1 << 30)) != 0) {
            return 0; // auto-repeat
        }
        if (message == WM_SYSKEYDOWN && wParam == VK_F4) {
            break; // let DefWindowProc turn Alt+F4 into the standard WM_CLOSE
        }
        EnqueueKey(window, wParam, true);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        EnqueueKey(window, wParam, false);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ------------------------------------------------------------------ frame

FrameInput DrainInput() {
    FrameInput &pending = g_platform.pending;
    pending.shift = KeyDown(VK_SHIFT);
    pending.ctrl = KeyDown(VK_CONTROL);
    pending.alt = KeyDown(VK_MENU);
    pending.cmd = pending.ctrl;
    pending.fullscreen = g_platform.fullscreen;
    FrameInput input = pending;

    pending.panX = 0.0f;
    pending.panY = 0.0f;
    pending.zoomDelta = 0.0f;
    pending.orbitYaw = 0.0f;
    pending.orbitPitch = 0.0f;
    pending.mouseDeltaX = 0.0f;
    pending.mouseDeltaY = 0.0f;
    pending.magnification = 0.0f;
    pending.scrollX = 0.0f;
    pending.scrollY = 0.0f;
    pending.keyEventCount = 0;
    pending.droppedFileCount = 0;
    pending.openedFile = nullptr;
    return input;
}

// 0 on the first call after lastCounter is zeroed, which FrameUpdate reads as
// "the loop just started or resumed".
float TickDeltaTime() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_platform.lastCounter.QuadPart == 0) {
        g_platform.lastCounter = now;
        return 0.0f;
    }
    float seconds = (float)((double)(now.QuadPart - g_platform.lastCounter.QuadPart) /
                            (double)g_platform.counterFrequency.QuadPart);
    g_platform.lastCounter = now;
    return seconds;
}

// OLE drag and drop needs this thread in a single-threaded apartment, but
// miniaudio's AudioInit has already joined it to the multithreaded one, and
// joining an STA before AudioInit makes WASAPI fault during device creation.
// So after Init, leave the MTA (miniaudio's device threads keep it alive, and
// this thread makes no further audio COM calls) and join an STA.
void JoinSingleThreadedApartment() {
    APTTYPE apartment;
    APTTYPEQUALIFIER qualifier;
    if (SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) && apartment == APTTYPE_MTA &&
        qualifier == APTTYPEQUALIFIER_NONE) {
        CoUninitialize();
    }
    OleInitialize(nullptr);
}

HWND CreateMainWindow(HINSTANCE instance) {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC; // WGL needs a device context that outlives a paint
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    RegisterClassExW(&windowClass);

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    return CreateWindowExW(0, kWindowClassName, L"Renderer", WS_OVERLAPPEDWINDOW,
                           workArea.left, workArea.top, workArea.right - workArea.left,
                           workArea.bottom - workArea.top, nullptr, g_platform.menuBar, instance,
                           nullptr);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_platform.counterFrequency);

    // Zeroed once, never freed: the OS reclaims it when the process exits.
    g_platform.arena = ArenaCreate(calloc(1, kArenaSize), kArenaSize);
    for (int i = 0; i < kMaxDroppedFiles; ++i) {
        g_platform.droppedPathPointers[i] = g_platform.droppedPaths[i];
    }

    g_platform.menuBar = BuildMenuBar();
    g_platform.window = CreateMainWindow(instance);
    g_platform.pending.displayRefreshHz = (float)MonitorRefreshHz(g_platform.window);
    g_platform.presenter = PresenterCreate(g_platform.window);

    RECT client;
    GetClientRect(g_platform.window, &client);
    PresenterResize(g_platform.presenter, client.right, client.bottom);

    PlatformMenuHooks menuHooks = {MenuHookShowOpenDialog, MenuHookToggleFullscreen, MenuHookQuit,
                                   nullptr};
    Init(&g_platform.arena, PresenterGpuContext(g_platform.presenter), (float)client.right,
         (float)client.bottom, menuHooks);
    g_platform.ready = true;

    JoinSingleThreadedApartment();
    RegisterDragDrop(g_platform.window, &g_dropTarget);
    ShowWindow(g_platform.window, SW_SHOW);
    SetForegroundWindow(g_platform.window);

    g_platform.running = true;
    g_platform.active = true;
    while (g_platform.running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!g_platform.running) {
            break;
        }
        bool dragInProgress = g_platform.dragFramesLeft > 0;
        if ((!g_platform.active || g_platform.minimized) && !dragInProgress) {
            g_platform.lastCounter.QuadPart = 0; // no giant first frame on resume
            WaitMessage();
            continue;
        }
        if (dragInProgress && !g_platform.pending.dragHovering) {
            --g_platform.dragFramesLeft;
        }

        if (FrameUpdate(&g_platform.arena, TickDeltaTime(), DrainInput())) {
            FrameRender(&g_platform.arena, PresenterBeginFrame(g_platform.presenter));
            PresenterEndFrame(g_platform.presenter);
        } else {
            PresenterWaitVBlank(g_platform.presenter);
        }
    }

    RevokeDragDrop(g_platform.window);
    OleUninitialize();
    return 0;
}
