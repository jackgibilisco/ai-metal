#pragma once

// A minimal, generic reader for the Blender 5.x .blend file format: zstd
// decompression, the 17-byte file header, LargeBHead8 block headers, and
// SDNA (struct DNA) parsing so struct fields can be read by name without
// hardcoding byte offsets. Pure, portable C++ — no platform or Metal
// headers, like math3d.h — but does link against libzstd.
//
// Only the new-format (Blender 5.0+) header is supported; pre-5.0 .blend
// files use a different, unsupported header layout.

#include <cstdint>

struct BlendFile;

BlendFile *BlendFileOpen(const char *path);
void BlendFileClose(BlendFile *file);

// Finds the next data block whose two-letter ID code matches (e.g. "OB",
// "ME"), in file order. Pass previousBlock = nullptr to start from the
// first block. Returns the block's raw struct data, and, via
// outStructIndex, which SDNA struct describes its layout.
const void *BlendFileNextBlock(BlendFile *file, const char *idCode,
                                const void *previousBlock, int *outStructIndex);

// Reads a named field out of a struct instance (from BlendFileNextBlock or
// BlendFileFollowPointer). Field names must match the C declarator exactly,
// e.g. "loc[3]", "rotmode". Returns false if the field isn't present in
// this file's struct layout, leaving `out` untouched.
bool BlendFileReadFloatArray(BlendFile *file, const void *structData, int structIndex,
                              const char *fieldName, float *out, int count);
bool BlendFileReadInt(BlendFile *file, const void *structData, int structIndex,
                       const char *fieldName, int32_t *out);

// Follows a pointer-valued field (e.g. Object's `ID *data`, passed here as
// fieldName "data") to the block it points to. Returns nullptr if the
// field is absent, the pointer is null, or the target isn't in this file.
const void *BlendFileFollowPointer(BlendFile *file, const void *structData, int structIndex,
                                    const char *fieldName, int *outStructIndex);
