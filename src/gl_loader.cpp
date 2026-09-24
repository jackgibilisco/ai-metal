#include "gl_loader.h"

#include <cstdio>
#include <cstdlib>

#define GlDefine(type, name) type name = nullptr;
GL_FUNCTIONS(GlDefine)
#undef GlDefine

namespace {

// wglGetProcAddress returns null for the GL 1.1 entry points opengl32.dll
// exports directly, so those come from the module instead.
void *GlResolve(const char *name) {
    void *address = (void *)wglGetProcAddress(name);
    if (address != nullptr) {
        return address;
    }
    static HMODULE opengl32 = LoadLibraryA("opengl32.dll");
    return (void *)GetProcAddress(opengl32, name);
}

} // namespace

void GlLoadFunctions() {
#define GlLoad(type, name)                                                                         \
    name = (type)GlResolve(#name);                                                                 \
    if (name == nullptr) {                                                                         \
        fprintf(stderr, "OpenGL driver is missing %s\n", #name);                                   \
        exit(1);                                                                                   \
    }
    GL_FUNCTIONS(GlLoad)
#undef GlLoad
}
