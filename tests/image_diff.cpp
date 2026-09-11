// Compares two tests/render_parity.cpp output directories (reference first).
// Pass --no-gpu-timings when the test backend has no GPU timer: its timing
// lines are then skipped instead of compared.
// Fails when a case's image differs beyond the thresholds below, when the
// timings.txt lines differ, or when a scripted case renders identically to
// "default" in the reference (the script had no effect, so the case tests
// nothing). Writes <case>_diff.ppm into the second directory on failure.
//
// Built and run by `make parity-test`.

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// A pixel "differs" when any channel is off by more than kNoticeableDelta.
// A case fails if any channel is off by more than kMaxDelta, or if more than
// kMaxDifferingFraction of its pixels differ. Metal and GL agree to within
// 1/255 on Apple silicon; the slack is for other GPUs' rounding.
constexpr int kNoticeableDelta = 1;
constexpr int kMaxDelta = 4;
constexpr double kMaxDifferingFraction = 0.001;

constexpr int kMaxCases = 64;
constexpr int kMaxLine = 256;

struct Image {
    int width;
    int height;
    unsigned char *rgb;
};

bool ReadPpm(const char *path, Image *out) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    int maxValue = 0;
    if (fscanf(file, "P6 %d %d %d", &out->width, &out->height, &maxValue) != 3) {
        fclose(file);
        return false;
    }
    fgetc(file); // the single whitespace byte after the header
    size_t size = (size_t)out->width * out->height * 3;
    out->rgb = (unsigned char *)malloc(size);
    bool complete = fread(out->rgb, 1, size, file) == size;
    fclose(file);
    return complete;
}

void WritePpm(const char *path, const Image &image) {
    FILE *file = fopen(path, "wb");
    fprintf(file, "P6\n%d %d\n255\n", image.width, image.height);
    fwrite(image.rgb, 1, (size_t)image.width * image.height * 3, file);
    fclose(file);
}

int ReadLines(const char *path, char lines[][kMaxLine]) {
    FILE *file = fopen(path, "r");
    if (file == nullptr) {
        printf("missing %s\n", path);
        exit(1);
    }
    int count = 0;
    while (count < kMaxCases && fgets(lines[count], kMaxLine, file) != nullptr) {
        lines[count][strcspn(lines[count], "\n")] = '\0';
        count++;
    }
    fclose(file);
    return count;
}

bool SameImage(const Image &a, const Image &b) {
    return a.width == b.width && a.height == b.height &&
           memcmp(a.rgb, b.rgb, (size_t)a.width * a.height * 3) == 0;
}

void LoadCase(const char *dir, const char *caseName, Image *out) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, caseName);
    if (!ReadPpm(path, out)) {
        printf("cannot read %s\n", path);
        exit(1);
    }
}

// Returns true if `test` is within tolerance of `reference`.
bool CompareCase(const char *caseName, const Image &reference, const Image &test,
                 const char *testDir) {
    if (reference.width != test.width || reference.height != test.height) {
        printf("FAIL %-14s size %dx%d vs %dx%d\n", caseName, reference.width, reference.height,
               test.width, test.height);
        return false;
    }
    int pixelCount = reference.width * reference.height;
    Image diff = {reference.width, reference.height, (unsigned char *)malloc(pixelCount * 3)};
    int maxDelta = 0;
    int differingPixels = 0;
    for (int i = 0; i < pixelCount; ++i) {
        int pixelDelta = 0;
        for (int channel = 0; channel < 3; ++channel) {
            int delta = abs(reference.rgb[i * 3 + channel] - test.rgb[i * 3 + channel]);
            pixelDelta = delta > pixelDelta ? delta : pixelDelta;
            int amplified = delta * 4;
            diff.rgb[i * 3 + channel] = (unsigned char)(amplified > 255 ? 255 : amplified);
        }
        maxDelta = pixelDelta > maxDelta ? pixelDelta : maxDelta;
        if (pixelDelta > kNoticeableDelta) {
            differingPixels++;
        }
    }
    double differingFraction = (double)differingPixels / (double)pixelCount;
    bool pass = maxDelta <= kMaxDelta && differingFraction <= kMaxDifferingFraction;
    printf("%s %-14s maxDelta=%3d differing=%.4f%%\n", pass ? "ok  " : "FAIL", caseName, maxDelta,
           differingFraction * 100.0);
    if (!pass) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s_diff.ppm", testDir, caseName);
        WritePpm(path, diff);
    }
    free(diff.rgb);
    return pass;
}

} // namespace

int main(int argc, char **argv) {
    bool testHasGpuTimer = !(argc == 4 && strcmp(argv[3], "--no-gpu-timings") == 0);
    if (argc != 3 && testHasGpuTimer) {
        printf("usage: %s <reference-dir> <test-dir> [--no-gpu-timings]\n", argv[0]);
        return 1;
    }
    const char *referenceDir = argv[1];
    const char *testDir = argv[2];
    if (!testHasGpuTimer) {
        printf("skip timings: %s has no GPU timer\n", testDir);
    }

    static char referenceTimings[kMaxCases][kMaxLine];
    static char testTimings[kMaxCases][kMaxLine];
    char path[1024];
    snprintf(path, sizeof(path), "%s/timings.txt", referenceDir);
    int caseCount = ReadLines(path, referenceTimings);
    snprintf(path, sizeof(path), "%s/timings.txt", testDir);
    int testCaseCount = ReadLines(path, testTimings);

    int failures = 0;
    if (caseCount == 0) {
        printf("FAIL no cases in %s/timings.txt\n", referenceDir);
        failures++;
    }
    if (caseCount != testCaseCount) {
        printf("FAIL case count %d vs %d\n", caseCount, testCaseCount);
        failures++;
    }

    Image defaultImage = {};
    LoadCase(referenceDir, "default", &defaultImage);

    for (int i = 0; i < caseCount && i < testCaseCount; ++i) {
        char caseName[kMaxLine];
        sscanf(referenceTimings[i], "%255s", caseName);

        if (testHasGpuTimer && strcmp(referenceTimings[i], testTimings[i]) != 0) {
            printf("FAIL timings\n  reference: %s\n  test:      %s\n", referenceTimings[i],
                   testTimings[i]);
            failures++;
        }

        Image reference = {};
        Image test = {};
        LoadCase(referenceDir, caseName, &reference);
        LoadCase(testDir, caseName, &test);
        if (strcmp(caseName, "default") != 0 && SameImage(reference, defaultImage)) {
            printf("FAIL %-14s renders identically to default: its script had no effect\n",
                   caseName);
            failures++;
        }
        if (!CompareCase(caseName, reference, test, testDir)) {
            failures++;
        }
        free(reference.rgb);
        free(test.rgb);
    }

    if (failures > 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all %d cases match\n", caseCount);
    return 0;
}
