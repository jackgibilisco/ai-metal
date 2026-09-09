#include "timeline.h"

#include <cstdio>
#include <cstring>

#include "undo_stack.h"

namespace {

// Handle layout: [31:12] generation | [11:0] slot. Generation starts at 0 in a
// zeroed slot and is bumped on every (re)use, so a stale handle never matches.
constexpr uint32_t kSlotBits = 12;
constexpr uint32_t kSlotMask = (1u << kSlotBits) - 1u;

uint32_t MakeHandle(int slot, uint32_t generation) {
    return (generation << kSlotBits) | ((uint32_t)slot & kSlotMask);
}
uint32_t HandleSlot(uint32_t handle) { return handle & kSlotMask; }
uint32_t HandleGen(uint32_t handle) { return handle >> kSlotBits; }

enum {
    TL_CreateClip,
    TL_DeleteClip,
    TL_MoveClip,
    TL_TrimClip,
    TL_AddTrack,
    TL_RemoveTrack,
};

struct ClipSlot {
    bool used;
    uint32_t generation;
    TimelineClip clip;
};

struct TrackSlot {
    bool used;
    uint32_t generation;
    TimelineTrack track;
};

// One undo step's worth of before/after state. The undo stack passes handlers
// the shared context (a SceneState*), so the owning TimelineState* rides along
// in the payload. The stack stores the payload by value, so this is built on
// the stack at submit time, not kept in a ring here.
struct TimelineCmdPayload {
    TimelineState *timeline;
    int kind;
    uint32_t slot;
    uint32_t generation; // the (re)used slot's generation for this edit
    TimelineClip clipBefore;
    TimelineClip clipAfter;
    TimelineTrack trackBefore;
    TimelineTrack trackAfter;
};

} // namespace

struct TimelineState {
    TimelineTransport transport;
    ClipSlot clips[kMaxTimelineClips];
    TrackSlot tracks[kMaxTimelineTracks];
};

namespace {

const ClipSlot *FindClipSlotConst(const TimelineState *timeline, TimelineClipId id) {
    if (id == kInvalidTimelineClipId) {
        return nullptr;
    }
    uint32_t slot = HandleSlot(id);
    if (slot >= (uint32_t)kMaxTimelineClips) {
        return nullptr;
    }
    const ClipSlot *entry = &timeline->clips[slot];
    if (!entry->used || entry->generation != HandleGen(id)) {
        return nullptr;
    }
    return entry;
}

ClipSlot *FindClipSlot(TimelineState *timeline, TimelineClipId id) {
    return const_cast<ClipSlot *>(FindClipSlotConst(timeline, id));
}

const TrackSlot *FindTrackSlotConst(const TimelineState *timeline, TrackId id) {
    if (id == kInvalidTrackId) {
        return nullptr;
    }
    uint32_t slot = HandleSlot(id);
    if (slot >= (uint32_t)kMaxTimelineTracks) {
        return nullptr;
    }
    const TrackSlot *entry = &timeline->tracks[slot];
    if (!entry->used || entry->generation != HandleGen(id)) {
        return nullptr;
    }
    return entry;
}

int FirstFreeClip(const TimelineState *timeline) {
    for (int i = 0; i < kMaxTimelineClips; ++i) {
        if (!timeline->clips[i].used) {
            return i;
        }
    }
    return -1;
}

int FirstFreeTrack(const TimelineState *timeline) {
    for (int i = 0; i < kMaxTimelineTracks; ++i) {
        if (!timeline->tracks[i].used) {
            return i;
        }
    }
    return -1;
}

SelectionItem ClipSelectionItem(uint32_t slot, uint32_t generation) {
    SelectionItem item;
    item.kind = SelectionKind_Clip;
    item.id = MakeHandle((int)slot, generation);
    return item;
}

void ApplyPayload(SceneState *scene, void *payloadPtr, bool redo) {
    TimelineCmdPayload *payload = (TimelineCmdPayload *)payloadPtr;
    TimelineState *timeline = payload->timeline;

    switch (payload->kind) {
    case TL_CreateClip: {
        ClipSlot *entry = &timeline->clips[payload->slot];
        if (redo) {
            entry->used = true;
            entry->generation = payload->generation;
            entry->clip = payload->clipAfter;
        } else {
            entry->used = false;
            SceneSelectionRemove(scene, ClipSelectionItem(payload->slot, payload->generation));
        }
        break;
    }
    case TL_DeleteClip: {
        ClipSlot *entry = &timeline->clips[payload->slot];
        if (redo) {
            entry->used = false;
            SceneSelectionRemove(scene, ClipSelectionItem(payload->slot, payload->generation));
        } else {
            entry->used = true;
            entry->generation = payload->generation;
            entry->clip = payload->clipBefore;
        }
        break;
    }
    case TL_MoveClip:
    case TL_TrimClip: {
        ClipSlot *entry = &timeline->clips[payload->slot];
        entry->clip = redo ? payload->clipAfter : payload->clipBefore;
        break;
    }
    case TL_AddTrack: {
        TrackSlot *entry = &timeline->tracks[payload->slot];
        if (redo) {
            entry->used = true;
            entry->generation = payload->generation;
            entry->track = payload->trackAfter;
        } else {
            entry->used = false;
        }
        break;
    }
    case TL_RemoveTrack: {
        TrackSlot *entry = &timeline->tracks[payload->slot];
        if (redo) {
            entry->used = false;
        } else {
            entry->used = true;
            entry->generation = payload->generation;
            entry->track = payload->trackBefore;
        }
        break;
    }
    }
}

void TimelineCmdRedo(void *context, void *payload) {
    ApplyPayload((SceneState *)context, payload, true);
}
void TimelineCmdUndo(void *context, void *payload) {
    ApplyPayload((SceneState *)context, payload, false);
}

double NonNegative(double v) { return v < 0.0 ? 0.0 : v; }

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle + per-frame
// ---------------------------------------------------------------------------
TimelineState *TimelineInit(Arena *arena) {
    TimelineState *timeline = ArenaPushStruct(arena, TimelineState);
    // Arena memory is calloc'd: every slot starts unused, generations 0, the
    // transport stopped at t = 0. Only the scrub sentinel needs a non-zero seed.
    timeline->transport.playheadDrag = -1.0;
    return timeline;
}

void TimelineUpdate(TimelineState *timeline, FrameInput input, SceneState *scene, double deltaTime) {
    (void)input; // pointer interaction with the panel lives in ui.cpp

    if (timeline->transport.playheadDrag >= 0.0) {
        timeline->transport.time = timeline->transport.playheadDrag;
    } else if (timeline->transport.playing) {
        timeline->transport.time = NonNegative(timeline->transport.time + deltaTime);
    }

    if (scene == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxTimelineTracks; ++i) {
        if (!timeline->tracks[i].used) {
            continue;
        }
        const TimelineTrack &track = timeline->tracks[i].track;
        AudioSourceParams *params = SceneFindAudioSourceParams(scene, track.targetSource);
        if (params != nullptr) {
            params->mute = track.muted;
            params->solo = track.soloed;
        }
    }
}

bool TimelineIsPlaying(const TimelineState *timeline) { return timeline->transport.playing; }
double TimelineTime(const TimelineState *timeline) { return timeline->transport.time; }

// ---------------------------------------------------------------------------
// Transport intents
// ---------------------------------------------------------------------------
void TimelinePlay(TimelineState *timeline) { timeline->transport.playing = true; }
void TimelinePause(TimelineState *timeline) { timeline->transport.playing = false; }
void TimelineTogglePlay(TimelineState *timeline) {
    timeline->transport.playing = !timeline->transport.playing;
}

void TimelineStopToZero(TimelineState *timeline) {
    timeline->transport.playing = false;
    timeline->transport.time = 0.0;
    timeline->transport.playheadDrag = -1.0;
}

void TimelineSeek(TimelineState *timeline, double seconds) {
    timeline->transport.time = NonNegative(seconds);
}

void TimelineScrub(TimelineState *timeline, double seconds) {
    timeline->transport.playheadDrag = NonNegative(seconds);
    timeline->transport.time = timeline->transport.playheadDrag;
}

void TimelineScrubEnd(TimelineState *timeline) { timeline->transport.playheadDrag = -1.0; }

bool TimelineIsScrubbing(const TimelineState *timeline) {
    return timeline->transport.playheadDrag >= 0.0;
}

// ---------------------------------------------------------------------------
// Model reads
// ---------------------------------------------------------------------------
int TimelineTracks(const TimelineState *timeline, TimelineTrackRow *out, int maxOut) {
    int n = 0;
    for (int i = 0; i < kMaxTimelineTracks && n < maxOut; ++i) {
        if (!timeline->tracks[i].used) {
            continue;
        }
        out[n].id = MakeHandle(i, timeline->tracks[i].generation);
        out[n].track = timeline->tracks[i].track;
        ++n;
    }
    return n;
}

const TimelineTrack *TimelineFindTrack(const TimelineState *timeline, TrackId id) {
    const TrackSlot *entry = FindTrackSlotConst(timeline, id);
    return entry != nullptr ? &entry->track : nullptr;
}

int TimelineClips(const TimelineState *timeline, TimelineClipRow *out, int maxOut) {
    int n = 0;
    for (int i = 0; i < kMaxTimelineClips && n < maxOut; ++i) {
        if (!timeline->clips[i].used) {
            continue;
        }
        out[n].id = MakeHandle(i, timeline->clips[i].generation);
        out[n].clip = timeline->clips[i].clip;
        ++n;
    }
    return n;
}

const TimelineClip *TimelineFindClip(const TimelineState *timeline, TimelineClipId id) {
    const ClipSlot *entry = FindClipSlotConst(timeline, id);
    return entry != nullptr ? &entry->clip : nullptr;
}

SelectionItem TimelineClipSelectionItem(TimelineClipId id) {
    SelectionItem item;
    item.kind = SelectionKind_Clip;
    item.id = id;
    return item;
}

// ---------------------------------------------------------------------------
// Structural edits
// ---------------------------------------------------------------------------
TrackId TimelineAddTrack(TimelineState *timeline, SceneState *scene, EntityId targetSource,
                               const char *name) {
    int slot = FirstFreeTrack(timeline);
    if (slot < 0) {
        fprintf(stderr, "timeline: track pool full (%d), add dropped\n", kMaxTimelineTracks);
        return kInvalidTrackId;
    }
    uint32_t generation = timeline->tracks[slot].generation + 1;

    TimelineTrack track = {};
    snprintf(track.name, sizeof(track.name), "%s", (name != nullptr && name[0] != '\0') ? name
                                                                                        : "Track");
    track.targetSource = targetSource;

    TimelineCmdPayload storage = {};
    TimelineCmdPayload *payload = &storage;
    payload->timeline = timeline;
    payload->kind = TL_AddTrack;
    payload->slot = (uint32_t)slot;
    payload->generation = generation;
    payload->trackAfter = track;
    UndoStackSubmit(SceneUndoStack(scene), TimelineCmdRedo, TimelineCmdUndo, payload, sizeof *payload);
    return MakeHandle(slot, generation);
}

void TimelineRemoveTrack(TimelineState *timeline, SceneState *scene, TrackId id) {
    const TrackSlot *entry = FindTrackSlotConst(timeline, id);
    if (entry == nullptr) {
        return;
    }
    TimelineCmdPayload storage = {};
    TimelineCmdPayload *payload = &storage;
    payload->timeline = timeline;
    payload->kind = TL_RemoveTrack;
    payload->slot = HandleSlot(id);
    payload->generation = HandleGen(id);
    payload->trackBefore = entry->track;
    UndoStackSubmit(SceneUndoStack(scene), TimelineCmdRedo, TimelineCmdUndo, payload, sizeof *payload);
}

TimelineClipId TimelineAddClip(TimelineState *timeline, SceneState *scene, TrackId track,
                                        WavId wav, double startTime, double duration) {
    if (TimelineFindTrack(timeline, track) == nullptr) {
        fprintf(stderr, "timeline: create clip on a dead track, dropped\n");
        return kInvalidTimelineClipId;
    }
    int slot = FirstFreeClip(timeline);
    if (slot < 0) {
        fprintf(stderr, "timeline: clip pool full (%d), create dropped\n", kMaxTimelineClips);
        return kInvalidTimelineClipId;
    }
    uint32_t generation = timeline->clips[slot].generation + 1;

    TimelineClip clip = {};
    clip.track = track;
    clip.wav = wav;
    clip.startTime = NonNegative(startTime);
    clip.duration = NonNegative(duration);
    clip.trimIn = 0.0;
    clip.trimOut = 0.0;
    clip.gain = 1.0f;

    TimelineCmdPayload storage = {};
    TimelineCmdPayload *payload = &storage;
    payload->timeline = timeline;
    payload->kind = TL_CreateClip;
    payload->slot = (uint32_t)slot;
    payload->generation = generation;
    payload->clipAfter = clip;
    UndoStackSubmit(SceneUndoStack(scene), TimelineCmdRedo, TimelineCmdUndo, payload, sizeof *payload);
    return MakeHandle(slot, generation);
}

void TimelineDeleteClip(TimelineState *timeline, SceneState *scene, TimelineClipId id) {
    const ClipSlot *entry = FindClipSlotConst(timeline, id);
    if (entry == nullptr) {
        return;
    }
    TimelineCmdPayload storage = {};
    TimelineCmdPayload *payload = &storage;
    payload->timeline = timeline;
    payload->kind = TL_DeleteClip;
    payload->slot = HandleSlot(id);
    payload->generation = HandleGen(id);
    payload->clipBefore = entry->clip;
    UndoStackSubmit(SceneUndoStack(scene), TimelineCmdRedo, TimelineCmdUndo, payload, sizeof *payload);
}

void TimelineMoveClip(TimelineState *timeline, SceneState *scene, TimelineClipId id,
                            TrackId newTrack, double newStartTime) {
    ClipSlot *entry = FindClipSlot(timeline, id);
    if (entry == nullptr) {
        return;
    }
    if (TimelineFindTrack(timeline, newTrack) == nullptr) {
        return;
    }
    TimelineCmdPayload storage = {};
    TimelineCmdPayload *payload = &storage;
    payload->timeline = timeline;
    payload->kind = TL_MoveClip;
    payload->slot = HandleSlot(id);
    payload->generation = HandleGen(id);
    payload->clipBefore = entry->clip;
    payload->clipAfter = entry->clip;
    payload->clipAfter.track = newTrack;
    payload->clipAfter.startTime = NonNegative(newStartTime);
    UndoStackSubmit(SceneUndoStack(scene), TimelineCmdRedo, TimelineCmdUndo, payload, sizeof *payload);
}

void TimelineTrimClip(TimelineState *timeline, SceneState *scene, TimelineClipId id,
                            double trimIn, double trimOut, double duration) {
    ClipSlot *entry = FindClipSlot(timeline, id);
    if (entry == nullptr) {
        return;
    }
    TimelineCmdPayload storage = {};
    TimelineCmdPayload *payload = &storage;
    payload->timeline = timeline;
    payload->kind = TL_TrimClip;
    payload->slot = HandleSlot(id);
    payload->generation = HandleGen(id);
    payload->clipBefore = entry->clip;
    payload->clipAfter = entry->clip;
    payload->clipAfter.startTime =
        NonNegative(entry->clip.startTime + (NonNegative(trimIn) - entry->clip.trimIn));
    payload->clipAfter.trimIn = NonNegative(trimIn);
    payload->clipAfter.trimOut = NonNegative(trimOut);
    payload->clipAfter.duration = NonNegative(duration);
    UndoStackSubmit(SceneUndoStack(scene), TimelineCmdRedo, TimelineCmdUndo, payload, sizeof *payload);
}

// ---------------------------------------------------------------------------
// Active-clip query
// ---------------------------------------------------------------------------
namespace {

int CollectVoices(const TimelineState *timeline, double time, bool filterSource, EntityId source,
                  TimelineVoice *out, int maxOut) {
    int count = 0;
    for (int i = 0; i < kMaxTimelineClips && count < maxOut; ++i) {
        if (!timeline->clips[i].used) {
            continue;
        }
        const TimelineClip &clip = timeline->clips[i].clip;
        if (!WavIdValid(clip.wav)) {
            continue;
        }
        if (time < clip.startTime || time >= clip.startTime + clip.duration) {
            continue;
        }
        const TimelineTrack *track = TimelineFindTrack(timeline, clip.track);
        if (track == nullptr) {
            continue;
        }
        if (filterSource && track->targetSource != source) {
            continue;
        }
        TimelineVoice &voice = out[count++];
        voice.source = track->targetSource;
        voice.wav = clip.wav;
        voice.localOffset = clip.trimIn + (time - clip.startTime);
        voice.gain = clip.gain;
        voice.clip = MakeHandle(i, timeline->clips[i].generation);
    }
    return count;
}

} // namespace

int TimelineActiveVoices(const TimelineState *timeline, double time, TimelineVoice *out, int maxOut) {
    return CollectVoices(timeline, time, false, kInvalidEntityId, out, maxOut);
}

int TimelineActiveVoicesForSource(const TimelineState *timeline, EntityId source, double time,
                                  TimelineVoice *out, int maxOut) {
    return CollectVoices(timeline, time, true, source, out, maxOut);
}
