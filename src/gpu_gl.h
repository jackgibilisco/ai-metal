#pragma once

// OpenGL definitions of the opaque types in gpu.h. Included only by the GL
// backend (renderer_gl.cpp, ui_render_gl.cpp) and by whoever owns the GL
// context and the frame (platform_macos_gl.mm, tests/offscreen_gl.cpp).
// Every backend call assumes that owner's 4.1 core context is current.

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION // macOS deprecates OpenGL but still ships 4.1 core
#endif
#include <OpenGL/gl3.h>

struct GpuContext {};

// framebuffer 0 is the window's default framebuffer.
struct RenderTarget {
    GLuint framebuffer;
    int width;
    int height;
};
