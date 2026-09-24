#pragma once

// opengl32.dll exports only OpenGL 1.1, so every entry point renderer_gl.cpp
// and ui_render_gl.cpp use is resolved at run time against the current context.
// The pointers carry the plain GL names, which is why this header must be
// included instead of <GL/gl.h>, never beside it.

#include "third_party/glcorearb.h"

#define GL_FUNCTIONS(X)                                                                            \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                                     \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                       \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                           \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                 \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                                               \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                                         \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                 \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                             \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                           \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                                     \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                   \
    X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv)                                                     \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                     \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                     \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                       \
    X(PFNGLCULLFACEPROC, glCullFace)                                                               \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)                                         \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                       \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                   \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                                             \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                                             \
    X(PFNGLDISABLEPROC, glDisable)                                                                 \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                           \
    X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)                                                         \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)                                                       \
    X(PFNGLENABLEPROC, glEnable)                                                                   \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                 \
    X(PFNGLFINISHPROC, glFinish)                                                                   \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                                 \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                       \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                                             \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                           \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                 \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                                               \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                                         \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                 \
    X(PFNGLGETERRORPROC, glGetError)                                                               \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                       \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                               \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                         \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                           \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                         \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                         \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                                           \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                                         \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                       \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                                           \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                                     \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                                             \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                                             \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                                             \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                                                             \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                                           \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                                               \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                           \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                                         \
    X(PFNGLVIEWPORTPROC, glViewport)

#define GlDeclare(type, name) extern type name;
GL_FUNCTIONS(GlDeclare)
#undef GlDeclare

// Call once, with the GL 4.1 core context already current. Aborts naming the
// first entry point the driver does not provide.
void GlLoadFunctions();
