#pragma once

#include "gpu_gl.h"

// Compiles and links a GLSL program; "#version 410 core" is prepended to both
// sources. Aborts with the driver log on failure, like the Metal backend does
// on a pipeline error.
GLuint GlCreateProgram(const char *vertexSource, const char *fragmentSource, const char *label);
