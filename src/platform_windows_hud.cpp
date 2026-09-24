#include "platform_windows_hud.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "frame_stats.h"

namespace {

constexpr int kMargin = 10;
constexpr int kGraphWidth = FrameStats::kCapacity;
constexpr int kGraphHeight = 64;
constexpr int kLineCount = 5;
constexpr int kFontHeight = 12;
constexpr wchar_t kHudClassName[] = L"RendererDebugHud";

} // namespace

struct DebugHud {
    HWND window;
    HWND owner;
    HFONT font;
    int lineHeight;
    int panelWidth;
    int panelHeight;
    bool visible;
    float targetFrameMs;
    RendererPassTimings passTimings;
    FrameStats stats;
    LARGE_INTEGER lastRedraw;
    LARGE_INTEGER counterFrequency;
};

namespace {

void DrawGraph(HDC dc, const DebugHud *hud, RECT rect) {
    float targetMs = hud->targetFrameMs > 0.0f ? hud->targetFrameMs : 1000.0f / 60.0f;
    float maxMs = targetMs * 3.0f;
    int height = rect.bottom - rect.top;

    HBRUSH panel = CreateSolidBrush(RGB(40, 40, 40));
    FillRect(dc, &rect, panel);
    DeleteObject(panel);

    HBRUSH guide = CreateSolidBrush(RGB(90, 90, 90));
    for (int multiple = 1; multiple <= 2; ++multiple) {
        int y = rect.bottom - (int)((float)height * ((float)multiple * targetMs / maxMs));
        RECT line = {rect.left, y, rect.right, y + 1};
        FillRect(dc, &line, guide);
    }
    DeleteObject(guide);

    HBRUSH green = CreateSolidBrush(RGB(52, 199, 89));
    HBRUSH yellow = CreateSolidBrush(RGB(255, 204, 0));
    HBRUSH red = CreateSolidBrush(RGB(255, 59, 48));
    for (int i = 0; i < hud->stats.count; ++i) {
        float ms = FrameStatsSample(&hud->stats, i);
        int barHeight = (int)((float)height * std::min(ms / maxMs, 1.0f));
        int x = rect.right - hud->stats.count + i;
        RECT bar = {x, rect.bottom - barHeight, x + 1, rect.bottom};
        FillRect(dc, &bar, (ms <= targetMs) ? green : (ms <= 2.0f * targetMs) ? yellow : red);
    }
    DeleteObject(green);
    DeleteObject(yellow);
    DeleteObject(red);
}

void DrawHud(HDC dc, DebugHud *hud) {
    RECT panel = {0, 0, hud->panelWidth, hud->panelHeight};
    HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &panel, background);
    DeleteObject(background);

    float fps = 1000.0f / std::max(FrameStatsMeanMs(&hud->stats, 20), 0.001f);
    float avgMs = FrameStatsMeanMs(&hud->stats, FrameStats::kCapacity);
    float lowMs = FrameStatsOnePercentLowMs(&hud->stats);
    float lowFps = 1000.0f / std::max(lowMs, 0.001f);
    float displayHz = hud->targetFrameMs > 0.0f ? 1000.0f / hud->targetFrameMs : 0.0f;
    RendererPassTimings gpu = hud->passTimings;

    char lines[kLineCount][96];
    snprintf(lines[0], sizeof(lines[0]), "FPS %.0f  (%.0f Hz)", fps, displayHz);
    snprintf(lines[1], sizeof(lines[1]), "avg %.2f ms", avgMs);
    snprintf(lines[2], sizeof(lines[2]), "1%% low %.0f fps  %.2f ms", lowFps, lowMs);
    snprintf(lines[3], sizeof(lines[3]), "GPU %.2f ms", gpu.totalMs);
    snprintf(lines[4], sizeof(lines[4]), " geo %.2f  ao %.2f  lit %.2f  fxaa %.2f", gpu.geometryMs,
             gpu.aoMs, gpu.lightingMs, gpu.fxaaMs);

    HGDIOBJ previousFont = SelectObject(dc, hud->font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < kLineCount; ++i) {
        TextOutA(dc, kMargin, kMargin + i * hud->lineHeight, lines[i], (int)strlen(lines[i]));
    }
    SelectObject(dc, previousFont);

    RECT graph = {kMargin, kMargin * 2 + kLineCount * hud->lineHeight, kMargin + kGraphWidth,
                  kMargin * 2 + kLineCount * hud->lineHeight + kGraphHeight};
    DrawGraph(dc, hud, graph);
}

LRESULT CALLBACK HudWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    DebugHud *hud = (DebugHud *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_PAINT && hud != nullptr) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        DrawHud(dc, hud);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

DebugHud *HudCreate(HWND owner) {
    WNDCLASSEXW hudClass = {};
    hudClass.cbSize = sizeof(hudClass);
    hudClass.lpfnWndProc = HudWindowProc;
    hudClass.hInstance = GetModuleHandleW(nullptr);
    hudClass.lpszClassName = kHudClassName;
    RegisterClassExW(&hudClass);

    DebugHud *hud = (DebugHud *)calloc(1, sizeof(DebugHud));
    hud->owner = owner;
    QueryPerformanceFrequency(&hud->counterFrequency);

    UINT dpi = GetDpiForWindow(owner);
    hud->font = CreateFontW(-MulDiv(kFontHeight, (int)dpi, 96), 0, 0, 0, FW_MEDIUM, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    HDC measureDc = GetDC(owner);
    HGDIOBJ previousFont = SelectObject(measureDc, hud->font);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(measureDc, &metrics);
    SIZE widest = {};
    const char *widestLine = " geo 00.00  ao 00.00  lit 00.00  fxaa 00.00";
    GetTextExtentPoint32A(measureDc, widestLine, (int)strlen(widestLine), &widest);
    SelectObject(measureDc, previousFont);
    ReleaseDC(owner, measureDc);

    hud->lineHeight = metrics.tmHeight + metrics.tmExternalLeading;
    hud->panelWidth = std::max((int)widest.cx, kGraphWidth) + kMargin * 2;
    hud->panelHeight = kLineCount * hud->lineHeight + kGraphHeight + kMargin * 3;

    // WS_EX_TRANSPARENT is the click-through that macOS gets from -hitTest:
    // returning nil, so camera drags reach the viewport underneath.
    hud->window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                      WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                  kHudClassName, L"", WS_POPUP, 0, 0, hud->panelWidth,
                                  hud->panelHeight, owner, nullptr, hudClass.hInstance, nullptr);
    SetWindowLongPtrW(hud->window, GWLP_USERDATA, (LONG_PTR)hud);
    SetLayeredWindowAttributes(hud->window, 0, 200, LWA_ALPHA);
    HudFollowOwner(hud);
    return hud;
}

void HudSetVisible(DebugHud *hud, bool visible) {
    hud->visible = visible;
    ShowWindow(hud->window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) {
        HudFollowOwner(hud);
    }
}

bool HudVisible(const DebugHud *hud) { return hud->visible; }

HWND HudWindow(const DebugHud *hud) { return hud->window; }

void HudSetTargetFrameMs(DebugHud *hud, float targetFrameMs) {
    hud->targetFrameMs = targetFrameMs;
}

void HudPushFrameTime(DebugHud *hud, float deltaSeconds) {
    FrameStatsPush(&hud->stats, deltaSeconds);
    if (!hud->visible) {
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed =
        (double)(now.QuadPart - hud->lastRedraw.QuadPart) / (double)hud->counterFrequency.QuadPart;
    if (elapsed > 0.066) {
        hud->lastRedraw = now;
        InvalidateRect(hud->window, nullptr, FALSE);
    }
}

void HudSetPassTimings(DebugHud *hud, RendererPassTimings timings) { hud->passTimings = timings; }

void HudFollowOwner(DebugHud *hud) {
    POINT clientOrigin = {0, 0};
    ClientToScreen(hud->owner, &clientOrigin);
    SetWindowPos(hud->window, HWND_TOPMOST, clientOrigin.x, clientOrigin.y, hud->panelWidth,
                 hud->panelHeight, SWP_NOACTIVATE | (hud->visible ? 0u : SWP_NOZORDER));
}
