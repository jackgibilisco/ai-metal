// The Windows platform layer: window, input, native menu bar, borderless
// fullscreen, drag and drop, file import, and the demand-driven frame loop.
// It names no graphics API — platform_windows_gl.cpp owns the context and the
// frame target behind platform_windows_present.h — exactly as
// platform_macos.mm never names Metal.

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app.h"
#include "arena.h"
#include "frame_input.h"
#include "menu.h"
#include "platform_windows_hud.h"
#include "platform_windows_present.h"

namespace {

constexpr size_t kArenaSize = 192 * 1024 * 1024;
constexpr wchar_t kWindowClassName[] = L"RendererWindow";

// A mouse wheel reports one coarse notch at a time; scale it into the same
// range a trackpad pinch produces, as the macOS layer does.
constexpr float kMouseWheelZoom = 0.05f;
constexpr float kPinchPerNotch = 0.05f;

constexpr int kMaxDroppedFiles = 16;
constexpr int kMaxDropPathLength = 1024;

// Menu item ids carry the CommandId, offset clear of the system SC_* range.
constexpr UINT kMenuIdBase = 0x1000;

struct PlatformState {
    Arena arena;
    HWND window;
    Presenter *presenter;
    DebugHud *hud;
    HMENU menuBar;

    bool ready; // false until the presenter, the app, and the HUD all exist
    bool running;
    bool active;
    bool minimized;
    bool fullscreen;
    WINDOWPLACEMENT windowedPlacement;
    LONG windowedStyle;

    bool f3Down; // held chord prefix for the debug shortcuts

    FrameInput pending;
    int lastMouseX;
    int lastMouseY;
    char droppedPaths[kMaxDroppedFiles][kMaxDropPathLength];
    const char *droppedPathPointers[kMaxDroppedFiles];

    LARGE_INTEGER counterFrequency;
    LARGE_INTEGER lastCounter; // zeroed to re-prime the clock after a pause
};

PlatformState g_platform;

void Utf8ToWide(const char *utf8, wchar_t *out, int outCount) {
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outCount);
}

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

unsigned int ModifierBits() {
    unsigned int mods = 0;
    // There is no Cmd key here. Ctrl stands in for it, so every
    // ShortcutMod_Cmd binding in the command table fires on Ctrl.
    if (GetKeyState(VK_CONTROL) < 0) mods |= ShortcutMod_Cmd | ShortcutMod_Ctrl;
    if (GetKeyState(VK_SHIFT) < 0) mods |= ShortcutMod_Shift;
    if (GetKeyState(VK_MENU) < 0) mods |= ShortcutMod_Alt;
    return mods;
}

void EnqueueKey(HWND window, WPARAM virtualKey, bool pressed) {
    if (g_platform.pending.keyEventCount >= kMaxKeyEvents) {
        return;
    }
    // TranslateMessage has already queued this keystroke's WM_CHAR, so taking
    // it here gives the shifted character a text field wants. Keys that
    // produce none (and every key-up) fall back to the unshifted layout
    // character, which is what the shortcut table matches on.
    unsigned int codepoint = 0;
    MSG character;
    if (pressed && PeekMessageW(&character, window, WM_CHAR, WM_CHAR, PM_REMOVE)) {
        codepoint = (unsigned int)character.wParam;
    } else {
        codepoint = MapVirtualKeyW((UINT)virtualKey, MAPVK_VK_TO_CHAR) & 0x7FFFu;
    }
    if (NormalizedKeyCode(virtualKey) != Key_None) {
        codepoint = 0;
    }
    unsigned int mods = ModifierBits();
    if (g_platform.f3Down && virtualKey != VK_F3) {
        mods |= ShortcutMod_F3;
    }
    KeyEvent event = {NormalizedKeyCode(virtualKey), codepoint, mods, pressed};
    g_platform.pending.keyEvents[g_platform.pending.keyEventCount++] = event;
}

// ------------------------------------------------------------------ commands

void ToggleBorderlessFullscreen();

void MenuHookImportFile(void *context);
void MenuHookToggleFullscreen(void *context) {
    (void)context;
    ToggleBorderlessFullscreen();
}
void MenuHookQuit(void *context) {
    (void)context;
    g_platform.running = false;
}

// ---------------------------------------------------------------- menu bar

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
            const Command *command = CommandById(entry);
            wchar_t label[128];
            Utf8ToWide(command->label, label, 128);
            AppendMenuW(popup, MF_STRING, kMenuIdBase + (UINT)command->id, label);
        }
        wchar_t title[64];
        Utf8ToWide(layout.menus[i].title, title, 64);
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)popup, title);
    }
    return bar;
}

// The analogue of macOS's -validateMenuItem:, run just before a popup opens.
void UpdateMenuPopup(HMENU popup) {
    CommandContext ctx = AppCommandContext(&g_platform.arena);
    int count = GetMenuItemCount(popup);
    for (int i = 0; i < count; ++i) {
        UINT itemId = GetMenuItemID(popup, i);
        if (itemId == 0 || itemId == (UINT)-1) {
            continue;
        }
        const Command *command = CommandById((CommandId)(itemId - kMenuIdBase));
        if (command == nullptr) {
            continue;
        }
        if (command->isChecked != nullptr) {
            CheckMenuItem(popup, itemId,
                          MF_BYCOMMAND | (command->isChecked(ctx) ? MF_CHECKED : MF_UNCHECKED));
        }
        bool enabled = command->isEnabled == nullptr || command->isEnabled(ctx);
        EnableMenuItem(popup, itemId, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    }
}

// ---------------------------------------------------------------- fullscreen

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
    HudFollowOwner(g_platform.hud);
    HudSetTargetFrameMs(g_platform.hud,
                        1000.0f / (float)PresenterDisplayRefreshHz(g_platform.window));
}

void ToggleBorderlessFullscreen() {
    g_platform.fullscreen = !g_platform.fullscreen;
    if (g_platform.fullscreen) {
        g_platform.windowedPlacement.length = sizeof(g_platform.windowedPlacement);
        GetWindowPlacement(g_platform.window, &g_platform.windowedPlacement);
        g_platform.windowedStyle = GetWindowLongW(g_platform.window, GWL_STYLE);

        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(g_platform.window, MONITOR_DEFAULTTONEAREST), &monitor);

        SetMenu(g_platform.window, nullptr); // the in-app strip takes over
        SetWindowLongW(g_platform.window, GWL_STYLE,
                       g_platform.windowedStyle & ~(LONG)WS_OVERLAPPEDWINDOW);
        // Unlike macOS, covering the monitor exactly is the right thing here:
        // the 1px overhang there dodges a WindowServer direct-scanout path that
        // DWM has no equivalent of.
        SetWindowPos(g_platform.window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(g_platform.window, GWL_STYLE, g_platform.windowedStyle);
        SetMenu(g_platform.window, g_platform.menuBar);
        SetWindowPlacement(g_platform.window, &g_platform.windowedPlacement);
        SetWindowPos(g_platform.window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    ResizeToClient();
    AppRequestRender(&g_platform.arena);
}

// --------------------------------------------------------------- file import

void MenuHookImportFile(void *context) {
    (void)context;
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    COMDLG_FILTERSPEC filter = {L"Blender scene", L"*.blend"};
    dialog->SetFileTypes(1, &filter);
    dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

    if (SUCCEEDED(dialog->Show(g_platform.window))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR widePath = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath))) {
                char path[kMaxDropPathLength];
                WideCharToMultiByte(CP_UTF8, 0, widePath, -1, path, kMaxDropPathLength, nullptr,
                                    nullptr);
                CoTaskMemFree(widePath);
                ImportBlendFile(&g_platform.arena, path);
            }
            item->Release();
        }
    }
    dialog->Release();

    // The dialog ran its own message pump, so the frame clock is stale.
    g_platform.lastCounter.QuadPart = 0;
    AppRequestRender(&g_platform.arena);
}

// ------------------------------------------------------------- drag and drop

bool IsWavPath(const wchar_t *path) {
    const wchar_t *dot = wcsrchr(path, L'.');
    return dot != nullptr && _wcsicmp(dot, L".wav") == 0;
}

// Returns how many dropped files are wavs, and when `store` is true copies
// their paths into the platform's buffers for this frame's FrameInput.
int CollectWavFiles(IDataObject *dataObject, bool store) {
    FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium = {};
    if (FAILED(dataObject->GetData(&format, &medium))) {
        return 0;
    }
    HDROP drop = (HDROP)GlobalLock(medium.hGlobal);
    UINT fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

    int wavCount = 0;
    for (UINT i = 0; i < fileCount && wavCount < kMaxDroppedFiles; ++i) {
        wchar_t widePath[kMaxDropPathLength];
        DragQueryFileW(drop, i, widePath, kMaxDropPathLength);
        if (!IsWavPath(widePath)) {
            continue;
        }
        if (store) {
            WideCharToMultiByte(CP_UTF8, 0, widePath, -1, g_platform.droppedPaths[wavCount],
                                kMaxDropPathLength, nullptr, nullptr);
        }
        ++wavCount;
    }
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return wavCount;
}

void SetHoverPoint(POINTL screenPoint, int fileCount) {
    POINT client = {screenPoint.x, screenPoint.y};
    ScreenToClient(g_platform.window, &client);
    g_platform.pending.dragHovering = fileCount > 0;
    g_platform.pending.dragHoverX = (float)client.x;
    g_platform.pending.dragHoverY = (float)client.y;
    g_platform.pending.dragHoverFileCount = fileCount;
}

// COM mandates the vtable; this is the one place polymorphism is the API.
struct WavDropTarget : IDropTarget {
    int hoveredWavCount = 0;

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
        hoveredWavCount = CollectWavFiles(dataObject, false);
        SetHoverPoint(point, hoveredWavCount);
        *effect = hoveredWavCount > 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD *effect) override {
        SetHoverPoint(point, hoveredWavCount);
        *effect = hoveredWavCount > 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        hoveredWavCount = 0;
        g_platform.pending.dragHovering = false;
        g_platform.pending.dragHoverFileCount = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *dataObject, DWORD, POINTL point,
                                   DWORD *effect) override {
        int count = CollectWavFiles(dataObject, true);
        POINT client = {point.x, point.y};
        ScreenToClient(g_platform.window, &client);
        g_platform.pending.droppedFiles = g_platform.droppedPathPointers;
        g_platform.pending.droppedFileCount = count;
        g_platform.pending.dropX = (float)client.x;
        g_platform.pending.dropY = (float)client.y;
        g_platform.pending.dragHovering = false;
        g_platform.pending.dragHoverFileCount = 0;
        hoveredWavCount = 0;
        *effect = count > 0 ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
};

WavDropTarget g_dropTarget;

// -------------------------------------------------------------- window proc

void TrackMouse(LPARAM lParam) {
    g_platform.pending.mouseX = (float)GET_X_LPARAM(lParam);
    g_platform.pending.mouseY = (float)GET_Y_LPARAM(lParam);
}

void UpdateMouseCapture() {
    bool anyDown = g_platform.pending.mouseLeftDown || g_platform.pending.mouseRightDown ||
                   g_platform.pending.mouseMiddleDown;
    if (anyDown && GetCapture() != g_platform.window) {
        SetCapture(g_platform.window);
    } else if (!anyDown && GetCapture() == g_platform.window) {
        ReleaseCapture();
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_platform.running = false;
        return 0;

    case WM_ACTIVATEAPP:
        g_platform.active = wParam != FALSE;
        // The HUD is a topmost overlay, so it has to step aside with the app.
        if (g_platform.ready && HudVisible(g_platform.hud)) {
            ShowWindow(HudWindow(g_platform.hud), g_platform.active ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        return 0;

    case WM_SIZE:
        g_platform.minimized = wParam == SIZE_MINIMIZED;
        if (!g_platform.minimized && g_platform.ready) {
            ResizeToClient();
            AppRequestRender(&g_platform.arena);
        }
        return 0;

    case WM_MOVE:
    case WM_DISPLAYCHANGE:
        if (g_platform.ready) {
            HudFollowOwner(g_platform.hud);
            HudSetTargetFrameMs(g_platform.hud, 1000.0f / (float)PresenterDisplayRefreshHz(window));
        }
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
        AppRequestRender(&g_platform.arena);
        return 0;

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        // Windows reports physical pixels; macOS accumulates drag deltas in
        // points, so divide by the scale to keep the camera feel the same.
        float scale = (float)GetDpiForWindow(window) / 96.0f;
        float deltaX = (float)(x - g_platform.lastMouseX) / scale;
        float deltaY = (float)(y - g_platform.lastMouseY) / scale;
        g_platform.lastMouseX = x;
        g_platform.lastMouseY = y;
        TrackMouse(lParam);

        if (g_platform.pending.mouseRightDown) {
            if (GetKeyState(VK_SHIFT) < 0) {
                g_platform.pending.orbitYaw += deltaX;
                g_platform.pending.orbitPitch += deltaY;
            } else {
                g_platform.pending.panX += deltaX;
                g_platform.pending.panY += deltaY;
            }
        }
        if (g_platform.pending.mouseMiddleDown) {
            g_platform.pending.orbitYaw += deltaX;
            g_platform.pending.orbitPitch += deltaY;
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        TrackMouse(lParam);
        g_platform.pending.mouseLeftDown = message == WM_LBUTTONDOWN;
        UpdateMouseCapture();
        return 0;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        TrackMouse(lParam);
        g_platform.pending.mouseRightDown = message == WM_RBUTTONDOWN;
        UpdateMouseCapture();
        return 0;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        TrackMouse(lParam);
        g_platform.pending.mouseMiddleDown = message == WM_MBUTTONDOWN;
        UpdateMouseCapture();
        return 0;

    case WM_MOUSEWHEEL: {
        float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        // A precision touchpad sends a pinch as ctrl+wheel, which is also what
        // every Windows app treats as zoom, so that is where magnification
        // comes from here.
        if (GetKeyState(VK_CONTROL) < 0) {
            g_platform.pending.magnification += notches * kPinchPerNotch;
            g_platform.pending.zoomDelta += notches * kPinchPerNotch;
        } else {
            g_platform.pending.scrollY += notches;
            g_platform.pending.zoomDelta += notches * kMouseWheelZoom;
        }
        return 0;
    }

    case WM_MOUSEHWHEEL:
        g_platform.pending.scrollX += (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((lParam & (1 << 30)) != 0) {
            return 0; // auto-repeat
        }
        if (wParam == VK_ESCAPE && g_platform.fullscreen) {
            ToggleBorderlessFullscreen();
            return 0;
        }
        if (wParam == VK_F3) {
            g_platform.f3Down = true;
        }
        EnqueueKey(window, wParam, true);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        EnqueueKey(window, wParam, false);
        if (wParam == VK_F3) {
            g_platform.f3Down = false;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ------------------------------------------------------------------ frame

FrameInput DrainInput() {
    FrameInput input = g_platform.pending;
    input.fullscreen = g_platform.fullscreen;

    g_platform.pending.panX = 0.0f;
    g_platform.pending.panY = 0.0f;
    g_platform.pending.zoomDelta = 0.0f;
    g_platform.pending.orbitYaw = 0.0f;
    g_platform.pending.orbitPitch = 0.0f;
    g_platform.pending.magnification = 0.0f;
    g_platform.pending.scrollX = 0.0f;
    g_platform.pending.scrollY = 0.0f;
    g_platform.pending.keyEventCount = 0;
    g_platform.pending.droppedFileCount = 0;

    g_platform.pending.shift = GetKeyState(VK_SHIFT) < 0;
    g_platform.pending.ctrl = GetKeyState(VK_CONTROL) < 0;
    g_platform.pending.alt = GetKeyState(VK_MENU) < 0;
    g_platform.pending.cmd = g_platform.pending.ctrl;
    return input;
}

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

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_platform.counterFrequency);

    // Zeroed once, never freed: the OS reclaims it when the process exits.
    g_platform.arena = ArenaCreate(calloc(1, kArenaSize), kArenaSize);
    for (int i = 0; i < kMaxDroppedFiles; ++i) {
        g_platform.droppedPathPointers[i] = g_platform.droppedPaths[i];
    }

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
    g_platform.menuBar = BuildMenuBar();
    g_platform.window = CreateWindowExW(
        WS_EX_ACCEPTFILES, kWindowClassName, L"Renderer", WS_OVERLAPPEDWINDOW, workArea.left,
        workArea.top, workArea.right - workArea.left, workArea.bottom - workArea.top, nullptr,
        g_platform.menuBar, instance, nullptr);

    g_platform.presenter = PresenterCreate(g_platform.window);

    RECT client;
    GetClientRect(g_platform.window, &client);
    PresenterResize(g_platform.presenter, client.right, client.bottom);

    PlatformMenuHooks menuHooks = {MenuHookImportFile, MenuHookToggleFullscreen, MenuHookQuit,
                                  nullptr};
    Init(&g_platform.arena, PresenterGpuContext(g_platform.presenter), (float)client.right,
         (float)client.bottom, menuHooks);
    AppRequestRender(&g_platform.arena);

    g_platform.hud = HudCreate(g_platform.window);
    g_platform.ready = true;
    ResizeToClient();

    // COM comes up only now: it has to be after AudioInit, because joining an
    // apartment first makes miniaudio's WASAPI backend fault during Init.
    OleInitialize(nullptr);
    RegisterDragDrop(g_platform.window, &g_dropTarget);
    ShowWindow(g_platform.window, SW_SHOW);
    SetForegroundWindow(g_platform.window);

    g_platform.running = true;
    g_platform.active = true;
    bool paused = false;
    while (g_platform.running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!g_platform.running) {
            break;
        }
        if (!g_platform.active || g_platform.minimized) {
            paused = true;
            WaitMessage();
            continue;
        }
        if (paused) {
            paused = false;
            g_platform.lastCounter.QuadPart = 0; // the clock restarts at zero
            AppRequestRender(&g_platform.arena);
        }

        float deltaTime = TickDeltaTime();
        bool needsRender = FrameUpdate(&g_platform.arena, deltaTime, DrainInput());

        bool hudWanted = AppDebugHudVisible(&g_platform.arena);
        if (hudWanted != HudVisible(g_platform.hud)) {
            HudSetVisible(g_platform.hud, hudWanted);
            AppRequestRender(&g_platform.arena); // repopulate the frozen readout
        }

        if (!needsRender) {
            PresenterWaitVBlank(g_platform.presenter);
            continue;
        }
        if (deltaTime > 0.0f) {
            HudPushFrameTime(g_platform.hud, deltaTime);
        }
        FrameRender(&g_platform.arena, PresenterBeginFrame(g_platform.presenter));
        PresenterEndFrame(g_platform.presenter);
        if (hudWanted) {
            HudSetPassTimings(g_platform.hud, FrameGpuTimings(&g_platform.arena));
        }
    }

    RevokeDragDrop(g_platform.window);
    OleUninitialize();
    return 0;
}
