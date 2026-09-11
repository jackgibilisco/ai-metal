#include "ui_render.h"

#include "font_atlas.h"
#include "gl_shader.h"
#include "gpu_gl.h"

#include <cstddef>
#include <cstdlib>

namespace {

const char *kUiVertex = R"(
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in float inMode;

uniform vec2 inverseScreenSize;

out vec4 color;
out vec2 uv;
out float mode;

void main() {
    vec2 ndc = vec2(inPosition.x * inverseScreenSize.x * 2.0 - 1.0,
                    1.0 - inPosition.y * inverseScreenSize.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    color = inColor;
    uv = inUv;
    mode = inMode;
}
)";

const char *kUiFragment = R"(
in vec4 color;
in vec2 uv;
in float mode;

uniform sampler2D fontAtlas;

out vec4 outColor;

void main() {
    vec4 result = color;
    if (mode > 0.5) {
        float signedDistance = texture(fontAtlas, uv).r;
        float edgeWidth = max(fwidth(signedDistance), 0.0001);
        result.a *= smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, signedDistance);
    }
    outColor = result;
}
)";

constexpr int kMaxUiVertices = 65536;

// The atlas is uploaded top row first and sampled unflipped: its v runs
// top-down in GL exactly as in Metal.
GLuint BuildFontAtlas(UiFontMetrics *out) {
    int atlasWidth = 0;
    int atlasHeight = 0;
    uint8_t *pixels = BakeFontAtlas(out, &atlasWidth, &atlasHeight);

    GLuint atlas = 0;
    glGenTextures(1, &atlas);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlasWidth, atlasHeight, 0, GL_RED, GL_UNSIGNED_BYTE,
                 pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(pixels);
    return atlas;
}

} // namespace

struct UiRenderState {
    GLuint program;
    GLuint vertexArray;
    GLuint vertexBuffer;
    GLuint fontAtlas;
    UiFontMetrics fontMetrics;
};

UiRenderState *UiRenderInit(Arena *arena, GpuContext *gpu) {
    (void)gpu;
    UiRenderState *state = ArenaPushStruct(arena, UiRenderState);

    state->program = GlCreateProgram(kUiVertex, kUiFragment, "ui");
    glUseProgram(state->program);
    glUniform1i(glGetUniformLocation(state->program, "fontAtlas"), 0);

    glGenVertexArrays(1, &state->vertexArray);
    glBindVertexArray(state->vertexArray);
    glGenBuffers(1, &state->vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, state->vertexBuffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex),
                          (const void *)offsetof(UiVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UiVertex),
                          (const void *)offsetof(UiVertex, rgba));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex),
                          (const void *)offsetof(UiVertex, u));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(UiVertex),
                          (const void *)offsetof(UiVertex, mode));
    glBindVertexArray(0);

    state->fontAtlas = BuildFontAtlas(&state->fontMetrics);
    return state;
}

const UiFontMetrics *UiRenderFontMetrics(const UiRenderState *uiRender) {
    return &uiRender->fontMetrics;
}

void UiRenderEncode(UiRenderState *uiRender, RenderTarget *targetPtr, const UiVertex *vertices,
                    int vertexCount, float drawableWidth, float drawableHeight) {
    RenderTarget target = *targetPtr;
    if (vertexCount <= 0) {
        return;
    }
    if (vertexCount > kMaxUiVertices) {
        vertexCount = kMaxUiVertices;
    }
    glBindBuffer(GL_ARRAY_BUFFER, uiRender->vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(UiVertex) * kMaxUiVertices, nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(UiVertex) * vertexCount, vertices);

    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, target.width, target.height);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(uiRender->program);
    glUniform2f(glGetUniformLocation(uiRender->program, "inverseScreenSize"), 1.0f / drawableWidth,
                1.0f / drawableHeight);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, uiRender->fontAtlas);
    glBindVertexArray(uiRender->vertexArray);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    glDisable(GL_BLEND);
}
