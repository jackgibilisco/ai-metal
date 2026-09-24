#include "offscreen.h"

#include "gpu_gl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/platform_windows_present.h"

// Everything past context creation is the same as offscreen_gl.cpp: WGL just
// needs a window to hang the context off, where CGL needs nothing at all.

namespace {

GpuContext *g_gpu;
GLuint g_framebuffer;
GLuint g_colorRenderbuffer;
RenderTarget g_target;
int g_width;
int g_height;

void CheckGlErrors(const char *label) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("GL error 0x%x after %s\n", error, label);
        exit(1);
    }
}

} // namespace

GpuContext *OffscreenInit(int width, int height) {
    WNDCLASSEXW headlessClass = {};
    headlessClass.cbSize = sizeof(headlessClass);
    headlessClass.style = CS_OWNDC;
    headlessClass.lpfnWndProc = DefWindowProcW;
    headlessClass.hInstance = GetModuleHandleW(nullptr);
    headlessClass.lpszClassName = L"RendererHeadless";
    RegisterClassExW(&headlessClass);

    HWND window = CreateWindowExW(0, headlessClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1,
                                  1, nullptr, nullptr, headlessClass.hInstance, nullptr);
    g_gpu = PresenterGpuContext(PresenterCreate(window));

    glGenFramebuffers(1, &g_framebuffer);
    glGenRenderbuffers(1, &g_colorRenderbuffer);
    OffscreenResize(width, height);
    return g_gpu;
}

void OffscreenResize(int width, int height) {
    glBindRenderbuffer(GL_RENDERBUFFER, g_colorRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              g_colorRenderbuffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("offscreen framebuffer incomplete\n");
        exit(1);
    }
    g_width = width;
    g_height = height;
}

RenderTarget *OffscreenBeginFrame() {
    g_target.framebuffer = g_framebuffer;
    g_target.width = g_width;
    g_target.height = g_height;

    glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);
    float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, magenta);
    return &g_target;
}

void OffscreenEndFrame(uint8_t *rgbaTopLeft) {
    glFinish();
    CheckGlErrors("frame");
    if (rgbaTopLeft == nullptr) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, g_width, g_height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaTopLeft);

    size_t rowBytes = (size_t)g_width * 4;
    uint8_t *swapRow = (uint8_t *)malloc(rowBytes);
    for (int row = 0; row < g_height / 2; ++row) {
        uint8_t *top = rgbaTopLeft + row * rowBytes;
        uint8_t *bottom = rgbaTopLeft + (g_height - 1 - row) * rowBytes;
        memcpy(swapRow, top, rowBytes);
        memcpy(top, bottom, rowBytes);
        memcpy(bottom, swapRow, rowBytes);
    }
    free(swapRow);
}
