#include "gl_shader.h"

#include <cstdio>
#include <cstdlib>

namespace {

GLuint CompileShader(GLenum type, const char *source, const char *label) {
    const char *parts[2] = {"#version 410 core\n", source};
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 2, parts, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "Failed to compile %s %s shader:\n%s\n", label,
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        abort();
    }
    return shader;
}

} // namespace

GLuint GlCreateProgram(const char *vertexSource, const char *fragmentSource, const char *label) {
    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, label);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource, label);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        fprintf(stderr, "Failed to link %s program:\n%s\n", label, log);
        abort();
    }
    return program;
}
