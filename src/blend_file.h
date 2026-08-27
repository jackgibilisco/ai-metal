#pragma once

// A minimal, generic reader for the Blender 5.x .blend file format

#include <cstdint>

struct BlendFile;

BlendFile *BlendFileOpen(const char *path);
void BlendFileClose(BlendFile *file);

struct BlendBlock {
    const void *data = nullptr;
    int structIndex = -1;
    explicit operator bool() const { return data != nullptr; }
};

BlendBlock BlendFileNextBlock(BlendFile *file, const char *idCode,
                               BlendBlock previousBlock = {});

bool BlendFileReadFloatArray(BlendFile *file, BlendBlock block,
                              const char *fieldName, float *out, int count);
bool BlendFileReadInt(BlendFile *file, BlendBlock block,
                       const char *fieldName, int32_t *out);

BlendBlock BlendFileFollowPointer(BlendFile *file, BlendBlock block, const char *fieldName);
