// Renders scripted editor states through whichever graphics backend this
// binary was linked against (offscreen_metal.mm + the Metal renderer, or
// offscreen_gl.mm + the GL renderer). Writes one <case>.ppm per case plus
// timings.txt into the output directory; tests/image_diff.cpp compares two
// backends' directories.
//
// Built and run by `make parity-test`.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app.h"
#include "offscreen.h"

namespace {

constexpr size_t kArenaSize = 192 * 1024 * 1024;
constexpr int kWidth = 1280;
constexpr int kHeight = 800;
constexpr float kFrameSeconds = 1.0f / 60.0f;

// Set by --ao-off. SSAO samples the depth hemisphere with enough float work
// that two GPU vendors disagree on a few percent of the pixels, so the Windows
// build compares a set of images rendered with the AO pass disabled. On one
// machine (macOS, Metal vs GL) the full images still match.
bool g_forceAoOff;

typedef void (*CaseScript)(Arena *arena, int width, int height);

struct ParityCase {
    const char *name;
    CaseScript script;
    int width;
    int height;
    bool aoOnly; // the script only changes the AO view, so --ao-off skips it
};

// The pointer rests over the 3D viewport (top middle of the default dock
// layout, above the timeline) on the ground plane, so camera input and clicks
// reach the scene instead of a panel.
FrameInput ViewportInput(int width, int height) {
    FrameInput input = {};
    input.mouseX = (float)width * 0.5f;
    input.mouseY = (float)height * 0.3f;
    return input;
}

// Mirrors the platform frame loop: render whenever the update asks for it
// (rendering is also what pushes the UI's content rect to the renderer).
void Step(Arena *arena, FrameInput input) {
    if (FrameUpdate(arena, kFrameSeconds, input)) {
        FrameRender(arena, OffscreenBeginFrame());
        OffscreenEndFrame(nullptr);
    }
}

void ScriptNothing(Arena *, int, int) {}

void ScriptAoRaw(Arena *arena, int, int) { AppInvokeCommand(arena, Command_CycleAoDebug); }

void ScriptAoOff(Arena *arena, int, int) {
    AppInvokeCommand(arena, Command_CycleAoDebug);
    AppInvokeCommand(arena, Command_CycleAoDebug);
}

void ScriptFxaaOff(Arena *arena, int, int) { AppInvokeCommand(arena, Command_ToggleFxaa); }

void ScriptLayoutBounds(Arena *arena, int, int) {
    AppInvokeCommand(arena, Command_ToggleLayoutBounds);
}

void ScriptMenuStrip(Arena *arena, int, int) { AppInvokeCommand(arena, Command_ToggleMenuBar); }

void ScriptOrbitZoom(Arena *arena, int width, int height) {
    FrameInput input = ViewportInput(width, height);
    input.orbitYaw = 60.0f;
    input.orbitPitch = 20.0f;
    input.zoomDelta = 0.3f;
    Step(arena, input);
}

// Clicks Occluder B (the right-hand cube in the default 1280x800 layout) to
// select it, then presses '1' for the translate tool so the gizmo draws.
void ScriptGizmo(Arena *arena, int, int) {
    FrameInput input = {};
    input.mouseX = 728.0f;
    input.mouseY = 180.0f;
    Step(arena, input);
    input.mouseLeftDown = true;
    Step(arena, input);
    input.mouseLeftDown = false;
    Step(arena, input);

    input.keyEvents[0] = KeyEvent{Key_None, '1', 0, true};
    input.keyEventCount = 1;
    Step(arena, input);
    input.keyEvents[0].pressed = false;
    Step(arena, input);
}

const ParityCase kCases[] = {
    {"default", ScriptNothing, kWidth, kHeight},
    {"ao_raw", ScriptAoRaw, kWidth, kHeight, true},
    {"ao_off", ScriptAoOff, kWidth, kHeight, true},
    {"fxaa_off", ScriptFxaaOff, kWidth, kHeight},
    {"orbit_zoom", ScriptOrbitZoom, kWidth, kHeight},
    {"gizmo", ScriptGizmo, kWidth, kHeight},
    {"layout_bounds", ScriptLayoutBounds, kWidth, kHeight},
    {"menu_strip", ScriptMenuStrip, kWidth, kHeight},
    {"resize", ScriptNothing, 1600, 900},
};

void WritePpm(const char *path, const uint8_t *rgba, int width, int height) {
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        printf("cannot write %s\n", path);
        exit(1);
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        fwrite(&rgba[i * 4], 1, 3, file);
    }
    fclose(file);
}

void RunCase(GpuContext *gpu, const ParityCase &parityCase, const char *outputDir,
             FILE *timingsFile) {
    OffscreenResize(kWidth, kHeight);
    void *memory = calloc(1, kArenaSize);
    Arena arena = ArenaCreate(memory, kArenaSize);
    PlatformMenuHooks noHooks = {};
    Init(&arena, gpu, (float)kWidth, (float)kHeight, noHooks);

    int width = parityCase.width;
    int height = parityCase.height;
    if (width != kWidth || height != kHeight) {
        OffscreenResize(width, height);
        FrameResize(&arena, (float)width, (float)height);
    }

    // Idle frames either side of the script: the dock layout (and so the
    // renderer's content rect) settles only after the first rendered frame.
    Step(&arena, ViewportInput(width, height));
    Step(&arena, ViewportInput(width, height));
    parityCase.script(&arena, width, height);
    if (g_forceAoOff) {
        // Every case left under --ao-off starts at the normal view (0);
        // cycling twice reaches 2, AO disabled.
        AppInvokeCommand(&arena, Command_CycleAoDebug);
        AppInvokeCommand(&arena, Command_CycleAoDebug);
    }
    Step(&arena, ViewportInput(width, height));
    Step(&arena, ViewportInput(width, height));

    // Two frames: timings report the last *completed* frame, so the second
    // render guarantees the first frame's timings have resolved.
    uint8_t *pixels = (uint8_t *)malloc((size_t)width * height * 4);
    for (int frame = 0; frame < 2; ++frame) {
        FrameRender(&arena, OffscreenBeginFrame());
        OffscreenEndFrame(pixels);
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", outputDir, parityCase.name);
    WritePpm(path, pixels, width, height);
    free(pixels);

    RendererPassTimings timings = FrameGpuTimings(&arena);
    fprintf(timingsFile, "%s geometry=%d ao=%d lighting=%d fxaa=%d total=%d\n", parityCase.name,
            timings.geometryMs > 0.0f, timings.aoMs > 0.0f, timings.lightingMs > 0.0f,
            timings.fxaaMs > 0.0f, timings.totalMs > 0.0f);

    // Never freed: AudioInit starts an output unit whose IO thread keeps
    // reading this arena for the life of the process.
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        printf("usage: %s <output-dir> [--ao-off]\n", argv[0]);
        return 1;
    }
    const char *outputDir = argv[1];
    g_forceAoOff = argc == 3 && strcmp(argv[2], "--ao-off") == 0;
    GpuContext *gpu = OffscreenInit(kWidth, kHeight);

    char timingsPath[1024];
    snprintf(timingsPath, sizeof(timingsPath), "%s/timings.txt", outputDir);
    FILE *timingsFile = fopen(timingsPath, "w");
    if (timingsFile == nullptr) {
        printf("cannot write %s\n", timingsPath);
        return 1;
    }
    int caseCount = 0;
    for (const ParityCase &parityCase : kCases) {
        if (g_forceAoOff && parityCase.aoOnly) {
            continue;
        }
        RunCase(gpu, parityCase, outputDir, timingsFile);
        caseCount++;
    }
    fclose(timingsFile);
    printf("rendered %d cases into %s\n", caseCount, outputDir);
    return 0;
}
