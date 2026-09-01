#pragma once

// Portable audio API + spatial mixer. Pure C++: no Metal, no AppKit, no
// miniaudio types in this header. miniaudio owns the backend device; the
// implementation lives in audio.cpp (the single MINIAUDIO_IMPLEMENTATION TU).
//
// Lifecycle:
//   AudioInit()        once, from Init(), portable code
//   AudioLoadClip()    any time (Dan's .wav drop) - sub-allocates from a
//                      fixed PCM pool reserved at init, never grows
//   AudioUpdate()      once per frame, after the scene + timeline update
//   AudioShutdown()    once, on teardown (stops the device)

#include "arena.h"
#include "math3d.h"

struct SceneState;    // Eric  - listener + audio-source iteration, ScenePickRay
struct TimelineState; // Dan   - active-clip query at a transport time
struct AudioState;    // opaque; defined in audio.cpp

// Handle into the AudioClip registry (fixed capacity 128). Distinct from a
// timeline clip: this is one decoded .wav in memory. A negative index is the
// "load failed / empty slot" sentinel; never dereferenced by the mixer.
struct ClipId {
    int index;
};
constexpr ClipId kInvalidClipId = {-1};
inline bool ClipIdValid(ClipId id) { return id.index >= 0; }

constexpr int kAudioClipCapacity = 128;   // wav registry (plan pool cap)
constexpr int kAudioSourceCapacity = 256; // scene audio sources (plan pool cap)

// Embedded verbatim in Eric's AudioSource component. One of these per source
// entity; the mixer reads them every frame through the scene iteration API.
struct AudioSourceParams {
    ClipId clip;

    float gain;    // linear, pre-spatial; 1.0 = unity
    float minDist; // distance attenuation is full-gain within this radius
    float maxDist; // ...and fully silent beyond it
    float rolloff; // falloff curve exponent inside the band; 1.0 = linear,
                   // >1.0 = faster near/slower far. gainFactor =
                   // pow(clamp01((maxDist-d)/(maxDist-minDist)), rolloff)

    bool loop;
    bool spatial; // false: play 2D at `gain`, no distance / HRTF / occlusion
    bool mute;
    bool solo;    // any source soloed -> only soloed sources are audible
};

inline AudioSourceParams AudioSourceParamsDefault() {
    AudioSourceParams p = {};
    p.clip = kInvalidClipId;
    p.gain = 1.0f;
    p.minDist = 1.0f;
    p.maxDist = 25.0f;
    p.rolloff = 1.0f;
    p.loop = false;
    p.spatial = true;
    p.mute = false;
    p.solo = false;
    return p;
}

// pcmPoolBytes is carved from the arena once and holds every decoded clip's
// mono f32 PCM at the device sample rate. 64 MiB ~= 5.5 min total at 48 kHz
// (plan round 1: manager grows the main arena by this amount).
// On device-init failure returns a non-null state at a 48000 fallback rate
// with the output device stopped: clips still decode/resample (useful for
// tests and headless CI), the mixer just never reaches speakers. Logs once.
constexpr size_t kAudioPcmPoolBytes = 64 * 1024 * 1024;
AudioState *AudioInit(Arena *arena, size_t pcmPoolBytes);
void        AudioShutdown(AudioState *audio);

// The device's actual output rate, chosen by the backend and queried back
// from miniaudio. All clips are resampled to this on load.
int AudioDeviceSampleRate(const AudioState *audio);

// Decode `wavPath` (u8/s16/s24/s32/f32, mono or stereo), resample to the
// device rate, downmix stereo -> mono (logs a warning: spatial voices need
// mono), and store it in the PCM pool. Returns kInvalidClipId and logs on a
// missing file, an unreadable/parse-failed file, or a full registry/pool.
// Never crashes.
ClipId AudioLoadClip(AudioState *audio, const char *wavPath);

// Decoded length in seconds (frames / device rate). 0 for an invalid ClipId.
// Dan uses this to size a freshly dropped clip on the timeline.
double AudioClipDuration(const AudioState *audio, ClipId clip);

// Per-frame drive. Reads the active listener transform and every audio-source
// transform + AudioSourceParams from `scene`, asks `timeline` which clip is
// active on each source at `transportTime` (plus its local playback offset
// and gain), and hands a fresh per-voice parameter block to the audio thread
// (lock-free: back buffer + atomic publish). `transportTime` is seconds on
// the global transport clock; the manager only advances it while the
// timeline is playing, so a paused transport passes a stalled value and the
// mixer freezes every voice in place. Safe to call with scene or timeline
// null (mixer goes silent).
void AudioUpdate(AudioState *audio, const SceneState *scene,
                 const TimelineState *timeline, double transportTime);

// --- Testing hooks (decode + attenuation + HRTF before scene.h lands) ------
// Same internal mixer the miniaudio callback runs, but callable synchronously
// and fed by AudioDebugSetVoice instead of the scene.

constexpr int kAudioMaxVoices = 256;

void AudioDebugSetListener(AudioState *audio, Vec3 position, Vec3 forward, Vec3 up);
void AudioDebugSetVoice(AudioState *audio, int slot, ClipId clip, Vec3 worldPos,
                        AudioSourceParams params, double localTime, bool occluded);
void AudioDebugClearVoices(AudioState *audio);

// Render `frameCount` interleaved-stereo f32 frames (L,R,L,R,...) from the
// currently published voice set. Advances voice cursors exactly like the
// real callback.
void AudioRenderForTest(AudioState *audio, float *out, int frameCount);
