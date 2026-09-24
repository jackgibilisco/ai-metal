#pragma once

// OpenGL definitions of the opaque types in gpu.h. Included only by the GL
// backend (renderer_gl.cpp, ui_render_gl.cpp) and by whoever owns the GL
// context and the frame (platform_macos_gl.mm, platform_windows_gl.cpp,
// tests/offscreen_gl.cpp, tests/offscreen_wgl.cpp).
// Every backend call assumes that owner's 4.1 core context is current.

#ifdef _WIN32
#include "windows/gl_loader.h" // opengl32.dll exports only GL 1.1; the rest is loaded
#else
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION // macOS deprecates OpenGL but still ships 4.1 core
#endif
#include <OpenGL/gl3.h>
#endif

struct GpuContext {};

// framebuffer 0 is the window's default framebuffer.
struct RenderTarget {
    GLuint framebuffer;
    int width;
    int height;
};
