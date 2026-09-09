// Characterization test for timeline structural edits going through the shared
// undo stack. Headless. Pins current behavior across the undo-stack split.
//
// Build + run:
//   clang++ -std=c++17 -Wall -Wextra -I src tests/timeline_undo_test.cpp \
//     src/timeline.cpp src/scene.cpp src/arena.cpp src/undo_stack.cpp \
//     -o build/timeline_undo_test && build/timeline_undo_test

#include <cstdio>
#include <cstdlib>

#include "scene.h"
#include "timeline.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static Arena g_arena;

static void ResetArena() {
    size_t bytes = 128 * 1024 * 1024;
    static void *block = nullptr;
    if (block == nullptr) {
        block = malloc(bytes);
    }
    g_arena = ArenaCreate(block, bytes);
}

static int TrackCount(const TimelineState *timeline, const SceneState *scene) {
    static TimelineTrackRow rows[kMaxTimelineTracks];
    return TimelineTracks(timeline, scene, rows, kMaxTimelineTracks);
}
static int ClipCount(const TimelineState *timeline) {
    static TimelineClipRow rows[kMaxTimelineClips];
    return TimelineClips(timeline, rows, kMaxTimelineClips);
}

static void TestClipEditsShareSceneHistory() {
    ResetArena();
    SceneState *scene = SceneInit(&g_arena);
    TimelineState *timeline = TimelineInit(&g_arena);

    // A track is just an audio source: creating the source is enough.
    EntityId source = SceneCreateEntity(scene, EntityKind_AudioSource, "src");
    CHECK(TrackCount(timeline, scene) == 1);

    TimelineClipId clip = TimelineAddClip(timeline, scene, source, kInvalidWavId, 1.0, 2.0);
    CHECK(clip != kInvalidTimelineClipId);
    CHECK(ClipCount(timeline) == 1);

    // One shared history: undo removes the clip, then the source (its track).
    SceneUndo(scene);
    CHECK(ClipCount(timeline) == 0);
    SceneUndo(scene);
    CHECK(TrackCount(timeline, scene) == 0);
    CHECK(!SceneCanUndo(scene));

    SceneRedo(scene);
    SceneRedo(scene);
    CHECK(TrackCount(timeline, scene) == 1);
    CHECK(ClipCount(timeline) == 1);
}

static void TestMoveClipUndo() {
    ResetArena();
    SceneState *scene = SceneInit(&g_arena);
    TimelineState *timeline = TimelineInit(&g_arena);

    EntityId a = SceneCreateEntity(scene, EntityKind_AudioSource, "a");
    EntityId b = SceneCreateEntity(scene, EntityKind_AudioSource, "b");
    TimelineClipId clip = TimelineAddClip(timeline, scene, a, kInvalidWavId, 0.0, 1.0);

    TimelineMoveClip(timeline, scene, clip, b, 5.0);
    const TimelineClip *moved = TimelineFindClip(timeline, clip);
    CHECK(moved != nullptr && moved->track == b && moved->startTime == 5.0);

    SceneUndo(scene);
    const TimelineClip *back = TimelineFindClip(timeline, clip);
    CHECK(back != nullptr && back->track == a && back->startTime == 0.0);
}

int main() {
    TestClipEditsShareSceneHistory();
    TestMoveClipUndo();
    if (g_failures == 0) {
        std::printf("timeline_undo_test: all checks passed\n");
        return 0;
    }
    std::printf("timeline_undo_test: %d check(s) failed\n", g_failures);
    return 1;
}
