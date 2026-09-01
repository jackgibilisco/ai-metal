// Portable audio: miniaudio device init, .wav decode/resample, the AudioClip
// registry, and the spatial mixer (parametric HRTF + distance + occlusion).
// The mixer runs on miniaudio's audio thread; the main thread hands it a
// fresh per-voice parameter block each frame through a double-buffered,
// atomically-published array (no locks on the audio thread).

#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio.h"

#include "audio.h"
#include "scene.h"
#include "timeline.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// --- small Vec3 helpers (math3d.h owns Mat4 only; Eric will not take Vec3 ---
// additions this round, so keep these local) ------------------------------
static Vec3 Sub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
static float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 Cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static float Length(Vec3 a) { return sqrtf(Dot(a, a)); }
static Vec3 Normalized(Vec3 a) {
    float len = Length(a);
    if (len < 1e-6f) return Vec3{0.0f, 0.0f, 0.0f};
    return Vec3{a.x / len, a.y / len, a.z / len};
}

// --- data ---------------------------------------------------------------
struct AudioClip {
    float *pcm; // mono, device sample rate, into the PCM pool
    uint64_t frameCount;
    bool valid;
};

// Written by the main thread, read by the audio thread. POD, memcpy-able.
struct VoiceParams {
    bool active;
    bool spatial;
    int clipIndex;
    int key; // identity of what this slot plays; a change => reset runtime
    double startFrame; // clip-local playhead, device-rate frames
    float targetGain; // linear, folds in distance + occlusion attenuation
    float azimuth; // radians, 0 ahead, + to the listener's right
    float elevation; // radians, + up
    float lowpassHz; // combined head-shadow + occlusion cutoff target
};

struct Biquad {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
};

// Audio-thread-only state that must persist across callbacks.
constexpr int kDelayRing = 128; // >= max fractional ITD in samples, power of two
struct VoiceRuntime {
    int key;
    double cursor; // clip-rate frames
    float gain; // smoothed toward VoiceParams.targetGain
    float delay[kDelayRing]; // mono pre-ITD ring
    int delayWrite;
    float lpZ[2]; // one-pole lowpass state, [0]=far ear [1]=near ear
    Biquad notch[2]; // pinna elevation notch per ear
};

struct AudioState {
    ma_device device;
    bool deviceOk;
    int sampleRate;

    uint8_t *pcmPool;
    size_t pcmPoolSize;
    size_t pcmPoolUsed;

    AudioClip clips[kAudioClipCapacity];
    int clipCount;

    VoiceParams voiceBuf[2][kAudioMaxVoices];
    std::atomic<int> publishedBuffer; // index into voiceBuf that the audio thread reads
    VoiceRuntime runtime[kAudioMaxVoices];
    std::atomic<bool> frozen; // transport paused: audio thread emits silence, cursors held

    Vec3 debugListenerPos;
    Vec3 debugListenerFwd;
    Vec3 debugListenerUp;
};

// --- attenuation / DSP -------------------------------------------------
static float DistanceAttenuation(float dist, float minDist, float maxDist, float rolloff) {
    if (maxDist <= minDist) return dist <= minDist ? 1.0f : 0.0f;
    if (dist <= minDist) return 1.0f;
    if (dist >= maxDist) return 0.0f;
    float t = (maxDist - dist) / (maxDist - minDist); // 1 at minDist, 0 at maxDist
    return powf(t, rolloff > 0.0f ? rolloff : 1.0f);
}

static float OnePoleLowpass(float *state, float x, float cutoffHz, int rate) {
    if (cutoffHz >= rate * 0.5f) {
        *state = x;
        return x;
    }
    float a = expf(-2.0f * (float)M_PI * cutoffHz / (float)rate);
    *state = (1.0f - a) * x + a * (*state);
    return *state;
}

// RBJ peaking EQ with negative gain: a soft notch for the pinna elevation cue.
static void PeakNotchCoeffs(Biquad *q, float f0, float qFactor, float gainDb, int rate) {
    float A = powf(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / (float)rate;
    float cw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * qFactor);
    float a0 = 1.0f + alpha / A;
    q->b0 = (1.0f + alpha * A) / a0;
    q->b1 = (-2.0f * cw) / a0;
    q->b2 = (1.0f - alpha * A) / a0;
    q->a1 = (-2.0f * cw) / a0;
    q->a2 = (1.0f - alpha / A) / a0;
}

static float BiquadTick(Biquad *q, float x) {
    float y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
    q->x2 = q->x1;
    q->x1 = x;
    q->y2 = q->y1;
    q->y1 = y;
    return y;
}

static float ReadDelay(const VoiceRuntime *rt, float samples) {
    if (samples < 0.0f) samples = 0.0f;
    int whole = (int)samples;
    float frac = samples - (float)whole;
    int i0 = (rt->delayWrite - whole) & (kDelayRing - 1);
    int i1 = (i0 - 1) & (kDelayRing - 1);
    return rt->delay[i0] * (1.0f - frac) + rt->delay[i1] * frac;
}

// Parametric HRTF for one mono sample: Woodworth ITD + head-shadow ILD +
// pinna notch. delay[] is assumed already advanced+written by the caller.
static void Spatialize(VoiceRuntime *rt, const VoiceParams *vp, int rate, float *outL, float *outR) {
    float az = vp->azimuth;
    float theta = fabsf(az);
    if (theta > (float)M_PI) theta = (float)M_PI;

    const float headRadius = 0.0875f;
    const float speedOfSound = 343.0f;
    float woodworth = theta <= (float)M_PI * 0.5f
                          ? theta + sinf(theta)
                          : (float)M_PI - theta + sinf(theta);
    float itdSamples = (headRadius / speedOfSound) * woodworth * (float)rate;
    if (itdSamples > (float)(kDelayRing - 2)) itdSamples = (float)(kDelayRing - 2);

    float nearSig = ReadDelay(rt, 0.0f);
    float farSig = ReadDelay(rt, itdSamples);

    float lateral = fabsf(sinf(az)); // 0 straight ahead/behind, 1 fully to a side
    float farGain = 1.0f - 0.5f * lateral; // down to -6 dB on the shadowed ear

    float shadowCutoff = 20000.0f - (20000.0f - 2000.0f) * lateral;
    float farCutoff = fminf(shadowCutoff, vp->lowpassHz);
    farSig = OnePoleLowpass(&rt->lpZ[0], farSig, farCutoff, rate);
    nearSig = OnePoleLowpass(&rt->lpZ[1], nearSig, vp->lowpassHz, rate);

    nearSig = BiquadTick(&rt->notch[0], nearSig);
    farSig = BiquadTick(&rt->notch[1], farSig) * farGain;

    if (az >= 0.0f) { // source on the right: right ear is the near ear
        *outR = nearSig;
        *outL = farSig;
    } else {
        *outL = nearSig;
        *outR = farSig;
    }
}

// --- mixer ------------------------------------------------------------
static void MixInto(AudioState *audio, float *out, int frameCount) {
    memset(out, 0, sizeof(float) * 2 * frameCount);
    if (audio->frozen.load(std::memory_order_acquire)) return;

    const VoiceParams *voices = audio->voiceBuf[audio->publishedBuffer.load(std::memory_order_acquire)];
    int rate = audio->sampleRate;

    for (int v = 0; v < kAudioMaxVoices; ++v) {
        const VoiceParams *vp = &voices[v];
        if (!vp->active) continue;
        if (vp->clipIndex < 0 || vp->clipIndex >= audio->clipCount) continue;
        const AudioClip *clip = &audio->clips[vp->clipIndex];
        if (!clip->valid) continue;

        VoiceRuntime *rt = &audio->runtime[v];
        if (rt->key != vp->key) {
            memset(rt, 0, sizeof(*rt));
            rt->key = vp->key;
            rt->cursor = vp->startFrame;
        } else if (fabs(rt->cursor - vp->startFrame) > rate * 0.05) {
            rt->cursor = vp->startFrame; // scrub / seek: jump, accept a small click
        }

        if (vp->spatial) {
            float elevInfluence = sinf(vp->elevation);
            float rear = fabsf(vp->azimuth) > (float)M_PI * 0.5f ? 2000.0f : 0.0f;
            float notchHz = 8000.0f - 3000.0f * elevInfluence - rear;
            if (notchHz < 4000.0f) notchHz = 4000.0f;
            if (notchHz > 11000.0f) notchHz = 11000.0f;
            PeakNotchCoeffs(&rt->notch[0], notchHz, 2.0f, -9.0f, rate);
            PeakNotchCoeffs(&rt->notch[1], notchHz, 2.0f, -9.0f, rate);
        }

        for (int i = 0; i < frameCount; ++i) {
            rt->gain += (vp->targetGain - rt->gain) * 0.002f;

            float mono = 0.0f;
            uint64_t idx = (uint64_t)rt->cursor;
            if (idx < clip->frameCount) mono = clip->pcm[idx] * rt->gain;
            rt->cursor += 1.0; // clip is already at device rate

            rt->delayWrite = (rt->delayWrite + 1) & (kDelayRing - 1);
            rt->delay[rt->delayWrite] = mono;

            float l, r;
            if (vp->spatial) {
                Spatialize(rt, vp, rate, &l, &r);
            } else {
                l = mono;
                r = mono;
            }
            out[2 * i] += l;
            out[2 * i + 1] += r;
        }
    }
}

static void AudioDataCallback(ma_device *device, void *output, const void *input, ma_uint32 frameCount) {
    (void)input;
    MixInto((AudioState *)device->pUserData, (float *)output, (int)frameCount);
}

// --- init / teardown -------------------------------------------------
AudioState *AudioInit(Arena *arena, size_t pcmPoolBytes) {
    AudioState *audio = ArenaPushStruct(arena, AudioState);
    memset(audio, 0, sizeof(*audio));
    audio->publishedBuffer.store(0);
    audio->frozen.store(true);

    audio->pcmPoolSize = pcmPoolBytes;
    audio->pcmPool = (uint8_t *)ArenaPush(arena, pcmPoolBytes, 16);

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 0; // let the backend pick its native rate
    config.dataCallback = AudioDataCallback;
    config.pUserData = audio;

    if (ma_device_init(NULL, &config, &audio->device) != MA_SUCCESS) {
        fprintf(stderr, "audio: device init failed; running silent at 48000 Hz\n");
        audio->deviceOk = false;
        audio->sampleRate = 48000;
        return audio;
    }
    audio->deviceOk = true;
    audio->sampleRate = (int)audio->device.sampleRate;
    ma_device_start(&audio->device);
    return audio;
}

void AudioShutdown(AudioState *audio) {
    if (audio && audio->deviceOk) {
        ma_device_uninit(&audio->device);
        audio->deviceOk = false;
    }
}

int AudioDeviceSampleRate(const AudioState *audio) { return audio ? audio->sampleRate : 0; }

double AudioClipDuration(const AudioState *audio, ClipId clip) {
    if (!audio || !ClipIdValid(clip) || clip.index >= audio->clipCount) return 0.0;
    const AudioClip *c = &audio->clips[clip.index];
    if (!c->valid || audio->sampleRate <= 0) return 0.0;
    return (double)c->frameCount / (double)audio->sampleRate;
}

// --- clip registry --------------------------------------------------
static float *PcmPoolAlloc(AudioState *audio, uint64_t frameCount) {
    size_t bytes = (size_t)frameCount * sizeof(float);
    if (audio->pcmPoolUsed + bytes > audio->pcmPoolSize) return NULL;
    float *ptr = (float *)(audio->pcmPool + audio->pcmPoolUsed);
    audio->pcmPoolUsed += bytes;
    return ptr;
}

ClipId AudioLoadClip(AudioState *audio, const char *wavPath) {
    if (!audio) return kInvalidClipId;
    if (audio->clipCount >= kAudioClipCapacity) {
        fprintf(stderr, "audio: clip registry full (%d), '%s' dropped\n", kAudioClipCapacity, wavPath);
        return kInvalidClipId;
    }

    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, audio->sampleRate);
    ma_decoder decoder;
    if (ma_decoder_init_file(wavPath, &decoderConfig, &decoder) != MA_SUCCESS) {
        fprintf(stderr, "audio: cannot open/decode '%s'\n", wavPath);
        return kInvalidClipId;
    }

    int channels = (int)decoder.outputChannels;
    if (channels > 1) {
        fprintf(stderr, "audio: '%s' is %d-channel; downmixing to mono (spatial sources need mono)\n",
                wavPath, channels);
    }

    ma_uint64 totalFrames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
    if (totalFrames == 0) {
        fprintf(stderr, "audio: '%s' decoded to 0 frames\n", wavPath);
        ma_decoder_uninit(&decoder);
        return kInvalidClipId;
    }

    float *mono = PcmPoolAlloc(audio, totalFrames);
    if (!mono) {
        fprintf(stderr, "audio: PCM pool full (%zu MiB), '%s' dropped\n",
                audio->pcmPoolSize / (1024 * 1024), wavPath);
        ma_decoder_uninit(&decoder);
        return kInvalidClipId;
    }

    float scratch[2048];
    int scratchFrames = (int)(sizeof(scratch) / sizeof(float)) / (channels < 1 ? 1 : channels);
    uint64_t written = 0;
    while (written < totalFrames) {
        ma_uint64 want = totalFrames - written;
        if (want > (ma_uint64)scratchFrames) want = (ma_uint64)scratchFrames;
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&decoder, scratch, want, &got);
        if (got == 0) break;
        for (ma_uint64 f = 0; f < got; ++f) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) sum += scratch[f * channels + c];
            mono[written + f] = channels > 0 ? sum / (float)channels : 0.0f;
        }
        written += got;
    }
    ma_decoder_uninit(&decoder);

    for (uint64_t f = written; f < totalFrames; ++f) mono[f] = 0.0f;

    int index = audio->clipCount++;
    audio->clips[index].pcm = mono;
    audio->clips[index].frameCount = totalFrames;
    audio->clips[index].valid = true;
    return ClipId{index};
}

// --- per-frame update ---------------------------------------------------
static Vec3 ToListenerSpace(Vec3 worldOffset, Vec3 forward, Vec3 up) {
    Vec3 f = Normalized(forward);
    Vec3 right = Normalized(Cross(f, up));
    Vec3 trueUp = Cross(right, f);
    return Vec3{Dot(worldOffset, right), Dot(worldOffset, trueUp), -Dot(worldOffset, f)};
}

static int MakeVoiceKey(int sourceEntity, int clipIndex, int ordinal) {
    return (sourceEntity * 131071) ^ (clipIndex * 977) ^ (ordinal * 31);
}

// Fills one spatial (or 2D) voice from a source transform + params. Shared by
// AudioUpdate and the debug hooks so both paths compute identical parameters.
static void FillVoice(const AudioState *audio, VoiceParams *vp, Vec3 sourceWorldPos,
                      Vec3 listenerPos, Vec3 listenerFwd, Vec3 listenerUp, bool haveListener,
                      AudioSourceParams params, int clipIndex, double localOffsetSeconds,
                      float timelineGain, bool occluded, int key) {
    *vp = {};
    vp->active = true;
    vp->clipIndex = clipIndex;
    vp->key = key;
    vp->startFrame = localOffsetSeconds * audio->sampleRate;
    vp->lowpassHz = 20000.0f;

    float gain = params.gain * timelineGain;
    if (params.spatial && haveListener) {
        vp->spatial = true;
        Vec3 rel = ToListenerSpace(Sub(sourceWorldPos, listenerPos), listenerFwd, listenerUp);
        float dist = Length(rel);
        gain *= DistanceAttenuation(dist, params.minDist, params.maxDist, params.rolloff);
        vp->azimuth = atan2f(rel.x, -rel.z);
        float horiz = sqrtf(rel.x * rel.x + rel.z * rel.z);
        vp->elevation = atan2f(rel.y, horiz);
        if (occluded) {
            gain *= 0.5f;
            vp->lowpassHz = 800.0f;
        }
    }
    vp->targetGain = gain;
}

void AudioUpdate(AudioState *audio, const SceneState *scene, const TimelineState *timeline,
                 double transportTime) {
    if (!audio) return;

    bool playing = timeline ? TimelineIsPlaying(timeline) : false;
    audio->frozen.store(!playing, std::memory_order_release);
    if (!playing) return; // keep the last published voices; audio thread is frozen

    int front = audio->publishedBuffer.load(std::memory_order_relaxed);
    VoiceParams *back = audio->voiceBuf[front ^ 1];
    for (int v = 0; v < kAudioMaxVoices; ++v) back[v] = {};

    if (scene && timeline) {
        Vec3 listenerPos = {}, listenerFwd = {0, 0, -1}, listenerUp = {0, 1, 0};
        bool haveListener = SceneActiveListener(scene, &listenerPos, &listenerFwd, &listenerUp);

        SceneAudioSourceView sources[kAudioSourceCapacity];
        int sourceCount = SceneAudioSources(scene, sources, kAudioSourceCapacity);

        bool anySolo = false;
        for (int s = 0; s < sourceCount; ++s) {
            if (sources[s].params.solo && !sources[s].params.mute) anySolo = true;
        }

        TimelineVoice timelineVoices[kAudioMaxVoices];
        int activeCount = TimelineActiveVoices(timeline, transportTime, timelineVoices, kAudioMaxVoices);

        int voice = 0;
        for (int c = 0; c < activeCount && voice < kAudioMaxVoices; ++c) {
            if (!ClipIdValid(timelineVoices[c].wav)) continue;
            int clipIndex = timelineVoices[c].wav.index;
            if (clipIndex >= audio->clipCount || !audio->clips[clipIndex].valid) continue;

            const SceneAudioSourceView *src = NULL;
            for (int s = 0; s < sourceCount; ++s) {
                if (sources[s].entity == timelineVoices[c].source) {
                    src = &sources[s];
                    break;
                }
            }
            if (!src) continue;
            if (src->params.mute || (anySolo && !src->params.solo)) continue;

            bool occluded = false;
            if (src->params.spatial && haveListener) {
                Vec3 toSource = Sub(src->worldPos, listenerPos);
                float dist = Length(toSource);
                Ray ray = {listenerPos, Normalized(toSource)};
                PickResult hit;
                if (ScenePickRayExcluding(scene, ray, timelineVoices[c].source, &hit) &&
                    hit.distance < dist - 0.05f) {
                    occluded = true;
                }
            }

            FillVoice(audio, &back[voice], src->worldPos, listenerPos, listenerFwd, listenerUp,
                      haveListener, src->params, clipIndex, timelineVoices[c].localOffset,
                      timelineVoices[c].gain, occluded, (int)timelineVoices[c].clip);
            ++voice;
        }
        if (voice >= kAudioMaxVoices) {
            fprintf(stderr, "audio: voice cap %d reached; extra active clips dropped this frame\n",
                    kAudioMaxVoices);
        }
    }

    audio->publishedBuffer.store(front ^ 1, std::memory_order_release);
}

// --- testing hooks -------------------------------------------------
void AudioDebugSetListener(AudioState *audio, Vec3 position, Vec3 forward, Vec3 up) {
    if (!audio) return;
    audio->debugListenerPos = position;
    audio->debugListenerFwd = forward;
    audio->debugListenerUp = up;
}

void AudioDebugSetVoice(AudioState *audio, int slot, ClipId clip, Vec3 worldPos,
                        AudioSourceParams params, double localTime, bool occluded) {
    if (!audio || slot < 0 || slot >= kAudioMaxVoices) return;
    int front = audio->publishedBuffer.load(std::memory_order_relaxed);
    VoiceParams *back = audio->voiceBuf[front ^ 1];
    memcpy(back, audio->voiceBuf[front], sizeof(audio->voiceBuf[0]));

    int clipIndex = ClipIdValid(clip) ? clip.index : -1;
    FillVoice(audio, &back[slot], worldPos, audio->debugListenerPos, audio->debugListenerFwd,
              audio->debugListenerUp, true, params, clipIndex, localTime, 1.0f, occluded,
              MakeVoiceKey(slot, clipIndex, 0));

    audio->publishedBuffer.store(front ^ 1, std::memory_order_release);
    audio->frozen.store(false, std::memory_order_release);
}

void AudioDebugClearVoices(AudioState *audio) {
    if (!audio) return;
    int front = audio->publishedBuffer.load(std::memory_order_relaxed);
    VoiceParams *back = audio->voiceBuf[front ^ 1];
    for (int v = 0; v < kAudioMaxVoices; ++v) back[v] = {};
    audio->publishedBuffer.store(front ^ 1, std::memory_order_release);
}

void AudioRenderForTest(AudioState *audio, float *out, int frameCount) {
    if (!audio) return;
    bool wasFrozen = audio->frozen.exchange(false, std::memory_order_acq_rel);
    MixInto(audio, out, frameCount);
    audio->frozen.store(wasFrozen, std::memory_order_release);
}
