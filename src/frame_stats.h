#pragma once

// Pure C++ frame-timing ring buffer for the debug HUD. Header-only, no
// platform or Metal dependencies, in the same spirit as math3d.h. Times are
// stored in milliseconds; the HUD view in platform_macos.mm reads them.

#include <algorithm>

struct FrameStats {
    static constexpr int kCapacity = 240;
    float samplesMs[kCapacity];
    int count;
    int writeIndex;
};

inline void FrameStatsPush(FrameStats *stats, float deltaSeconds) {
    stats->samplesMs[stats->writeIndex] = deltaSeconds * 1000.0f;
    stats->writeIndex = (stats->writeIndex + 1) % FrameStats::kCapacity;
    if (stats->count < FrameStats::kCapacity) {
        stats->count += 1;
    }
}

// chronologicalIndex 0 is the oldest sample still retained.
inline float FrameStatsSample(const FrameStats *stats, int chronologicalIndex) {
    int oldest = (stats->count == FrameStats::kCapacity) ? stats->writeIndex : 0;
    return stats->samplesMs[(oldest + chronologicalIndex) % FrameStats::kCapacity];
}

inline float FrameStatsMeanMs(const FrameStats *stats, int lastN) {
    int n = std::min(lastN, stats->count);
    if (n == 0) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (int i = stats->count - n; i < stats->count; ++i) {
        sum += FrameStatsSample(stats, i);
    }
    return sum / (float)n;
}

// 99th-percentile frame time: the worst frame once the slowest ~1% are set
// aside, so a single hitch doesn't dominate it.
inline float FrameStatsOnePercentLowMs(const FrameStats *stats) {
    if (stats->count == 0) {
        return 0.0f;
    }
    float sorted[FrameStats::kCapacity];
    for (int i = 0; i < stats->count; ++i) {
        sorted[i] = FrameStatsSample(stats, i);
    }
    std::sort(sorted, sorted + stats->count);
    int index = stats->count - 1 - stats->count / 100;
    return sorted[index];
}
