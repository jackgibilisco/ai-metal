#pragma once

// The timeline model + transport. Pure C++: no Metal, no AppKit. Tracks bind to
// scene audio sources; clips place a decoded .wav on a track starting at a
// transport time. The transport clock is the editor's global clock:
// while it plays, sim/animation/audio time advances; while paused it holds.
//
// Every structural edit (create/move/trim/delete clip, add/remove track)
// submits one command to the shared UndoStack (via SceneUndoStack), so one
// edit == one undo step in the same history as scene edits. The timeline panel
// in ui.cpp calls the TimelineAdd* / TimelineMove* / etc functions on
// drag-release / drop; the mixer calls TimelineActiveVoices* each frame.

#include <cstdint>

#include "arena.h"
#include "audio.h" // WavId
#include "frame_input.h"
#include "scene.h" // EntityId, SceneState, Command

struct TimelineState; // opaque; defined in timeline.cpp

// Handles pack [31:12] generation | [11:0] slot, so a slot reused after a
// delete never aliases an old handle. 0 is always invalid. Clip handles also
// ride inside SelectionItem.id (a uint32_t) in the shared selection set.
typedef uint32_t TrackId;
typedef uint32_t TimelineClipId;
constexpr TrackId kInvalidTrackId = 0;
constexpr TimelineClipId kInvalidTimelineClipId = 0;

constexpr int kMaxTimelineTracks = 64;
constexpr int kMaxTimelineClips = 1024;

struct TimelineTrack {
    char name[32];
    EntityId targetSource; // the scene audio source this lane drives
    bool muted;
    bool soloed;
};

struct TimelineClip {
    TrackId track;
    WavId wav;       // decoded-wav handle from AudioLoadWav
    double startTime; // transport seconds where playback begins
    double duration;  // seconds occupied on the timeline (already trim-adjusted)
    double trimIn;    // seconds skipped from the head of the source wav
    double trimOut;   // seconds skipped from the tail of the source wav
    float gain;       // per-clip linear gain (pre track mute/solo)
};

struct TimelineTransport {
    bool playing;
    double time;         // global transport clock, seconds
    double playheadDrag; // scrub target for this frame; < 0 means "not scrubbing"
};

// ---------------------------------------------------------------------------
// Lifecycle + per-frame
// ---------------------------------------------------------------------------
TimelineState *TimelineInit(Arena *arena);

// Called once per frame by app.cpp BEFORE the sim-time gate. Consumes a
// pending scrub, advances the clock by deltaTime while playing, and mirrors
// each track's mute/solo into its source's AudioSourceParams. Panel-driven
// edits are submitted from ui.cpp, not here.
void TimelineUpdate(TimelineState *timeline, FrameInput input, SceneState *scene, double deltaTime);

bool TimelineIsPlaying(const TimelineState *timeline);
double TimelineTime(const TimelineState *timeline);

// ---------------------------------------------------------------------------
// Transport intents (transport buttons; not undoable)
// ---------------------------------------------------------------------------
void TimelinePlay(TimelineState *timeline);
void TimelinePause(TimelineState *timeline);
void TimelineTogglePlay(TimelineState *timeline);
void TimelineStopToZero(TimelineState *timeline);
void TimelineSeek(TimelineState *timeline, double seconds); // one-shot jump (stop / ruler click)
void TimelineScrub(TimelineState *timeline, double seconds); // playhead grabbed and dragged
void TimelineScrubEnd(TimelineState *timeline);              // playhead released
bool TimelineIsScrubbing(const TimelineState *timeline);

// ---------------------------------------------------------------------------
// Model reads: fill a caller buffer, return the count written (the panel
// draws from these). Same shape as SceneMeshRenderers / TimelineActiveVoices.
// ---------------------------------------------------------------------------
struct TimelineTrackRow {
    TrackId id;
    TimelineTrack track;
};
int TimelineTracks(const TimelineState *timeline, TimelineTrackRow *out, int maxOut); // lane order

struct TimelineClipRow {
    TimelineClipId id;
    TimelineClip clip;
};
int TimelineClips(const TimelineState *timeline, TimelineClipRow *out, int maxOut);

const TimelineTrack *TimelineFindTrack(const TimelineState *timeline, TrackId id); // null if stale
const TimelineClip *TimelineFindClip(const TimelineState *timeline, TimelineClipId id); // null if stale

// The SelectionItem a clip occupies in the shared selection set.
SelectionItem TimelineClipSelectionItem(TimelineClipId id);

// ---------------------------------------------------------------------------
// Structural edits (undoable; each submits exactly one Command to `scene`)
// ---------------------------------------------------------------------------
// Return the new handle, or kInvalid* on a full pool (drop + log).
TrackId TimelineAddTrack(TimelineState *timeline, SceneState *scene, EntityId targetSource,
                               const char *name);
void TimelineRemoveTrack(TimelineState *timeline, SceneState *scene, TrackId id);

// The panel has already called AudioLoadWav; it passes the wav handle and the
// clip length it wants (typically the wav's full duration).
TimelineClipId TimelineAddClip(TimelineState *timeline, SceneState *scene, TrackId track,
                                        WavId wav, double startTime, double duration);
void TimelineDeleteClip(TimelineState *timeline, SceneState *scene, TimelineClipId id);
void TimelineMoveClip(TimelineState *timeline, SceneState *scene, TimelineClipId id,
                            TrackId newTrack, double newStartTime);
// Trimming the head (a larger trimIn) slides startTime by the same delta, so the
// audio that survives the trim keeps its position on the timeline. Trimming the
// tail (trimOut / duration) leaves startTime alone.
void TimelineTrimClip(TimelineState *timeline, SceneState *scene, TimelineClipId id,
                            double trimIn, double trimOut, double duration);

// ---------------------------------------------------------------------------
// Active-clip query for the mixer
// ---------------------------------------------------------------------------
// A clip is a voice at time T when startTime <= T < startTime + duration and
// its wav handle is valid. Overlapping clips on one source produce multiple
// voices. localOffset = trimIn + (T - startTime). gain is the clip gain only;
// track mute/solo is applied by the mixer via the mirrored AudioSourceParams.
struct TimelineVoice {
    EntityId source;
    WavId wav;
    double localOffset;
    float gain;
    TimelineClipId clip;
};
int TimelineActiveVoices(const TimelineState *timeline, double time, TimelineVoice *out, int maxOut);
int TimelineActiveVoicesForSource(const TimelineState *timeline, EntityId source, double time,
                                  TimelineVoice *out, int maxOut);
