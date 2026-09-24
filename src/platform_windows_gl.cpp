#include "platform_windows_present.h"

#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>

#include "gpu_gl.h"

// WGL bootstrapping: a pixel format can only be set once per window, and the
// ARB entry points that choose a modern one can only be resolved through a
// context that already exists. So a throwaway window carries a legacy context
// just long enough to look them up.

#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#define WGL_PIXEL_TYPE_ARB 0x2013
#define WGL_TYPE_RGBA_ARB 0x202B
#define WGL_ACCELERATION_ARB 0x2003
#define WGL_FULL_ACCELERATION_ARB 0x2027
#define WGL_COLOR_BITS_ARB 0x2014
#define WGL_ALPHA_BITS_ARB 0x201B
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef BOOL(WINAPI *PFNWGLCHOOSEPIXELFORMATARBPROC)(HDC, const int *, const FLOAT *, UINT, int *,
                                                     UINT *);
typedef HGLRC(WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int *);
typedef BOOL(WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int);

struct Presenter {
    HWND window;
    HDC deviceContext;
    HGLRC renderContext;
    GpuContext gpu;
    RenderTarget target;
};

namespace {

PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB;
PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB;
PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;

void Fail(const char *what) {
    fprintf(stderr, "%s failed (0x%lx)\n", what, GetLastError());
    exit(1);
}

void LoadWglExtensions() {
    WNDCLASSEXW dummyClass = {};
    dummyClass.cbSize = sizeof(dummyClass);
    dummyClass.lpfnWndProc = DefWindowProcW;
    dummyClass.hInstance = GetModuleHandleW(nullptr);
    dummyClass.lpszClassName = L"RendererWglBootstrap";
    RegisterClassExW(&dummyClass);

    HWND dummy = CreateWindowExW(0, dummyClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                                 nullptr, nullptr, dummyClass.hInstance, nullptr);
    HDC dummyDc = GetDC(dummy);

    PIXELFORMATDESCRIPTOR descriptor = {};
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    SetPixelFormat(dummyDc, ChoosePixelFormat(dummyDc, &descriptor), &descriptor);

    HGLRC dummyContext = wglCreateContext(dummyDc);
    wglMakeCurrent(dummyDc, dummyContext);

    wglChoosePixelFormatARB =
        (PFNWGLCHOOSEPIXELFORMATARBPROC)wglGetProcAddress("wglChoosePixelFormatARB");
    wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (wglChoosePixelFormatARB == nullptr || wglCreateContextAttribsARB == nullptr) {
        Fail("this driver has no modern WGL context creation");
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(dummyContext);
    ReleaseDC(dummy, dummyDc);
    DestroyWindow(dummy);
    UnregisterClassW(dummyClass.lpszClassName, dummyClass.hInstance);
}

} // namespace

Presenter *PresenterCreate(HWND window) {
    LoadWglExtensions();

    Presenter *presenter = (Presenter *)calloc(1, sizeof(Presenter));
    presenter->window = window;
    presenter->deviceContext = GetDC(window);

    // No depth or stencil on the default framebuffer: the renderer draws the
    // scene into its own targets and only composites here.
    const int formatAttributes[] = {
        WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
        WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
        WGL_DOUBLE_BUFFER_ARB,  GL_TRUE,
        WGL_PIXEL_TYPE_ARB,     WGL_TYPE_RGBA_ARB,
        WGL_ACCELERATION_ARB,   WGL_FULL_ACCELERATION_ARB,
        WGL_COLOR_BITS_ARB,     24,
        WGL_ALPHA_BITS_ARB,     8,
        0,
    };
    int pixelFormat = 0;
    UINT formatCount = 0;
    if (!wglChoosePixelFormatARB(presenter->deviceContext, formatAttributes, nullptr, 1,
                                 &pixelFormat, &formatCount) ||
        formatCount == 0) {
        Fail("wglChoosePixelFormatARB");
    }
    PIXELFORMATDESCRIPTOR chosen = {};
    DescribePixelFormat(presenter->deviceContext, pixelFormat, sizeof(chosen), &chosen);
    SetPixelFormat(presenter->deviceContext, pixelFormat, &chosen);

    const int contextAttributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 1,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0,
    };
    presenter->renderContext =
        wglCreateContextAttribsARB(presenter->deviceContext, nullptr, contextAttributes);
    if (presenter->renderContext == nullptr) {
        Fail("no OpenGL 4.1 core context");
    }
    wglMakeCurrent(presenter->deviceContext, presenter->renderContext);
    GlLoadFunctions();

    if (wglSwapIntervalEXT != nullptr) {
        wglSwapIntervalEXT(1);
    }
    return presenter;
}

GpuContext *PresenterGpuContext(Presenter *presenter) { return &presenter->gpu; }

void PresenterResize(Presenter *presenter, int width, int height) {
    presenter->target.width = width;
    presenter->target.height = height;
}

RenderTarget *PresenterBeginFrame(Presenter *presenter) {
    presenter->target.framebuffer = 0;
    return &presenter->target;
}

void PresenterEndFrame(Presenter *presenter) { SwapBuffers(presenter->deviceContext); }

void PresenterWaitVBlank(Presenter *presenter) {
    (void)presenter;
    DwmFlush();
}

int PresenterDisplayRefreshHz(HWND window) {
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
