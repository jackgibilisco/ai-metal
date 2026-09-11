#pragma once

// CPU bake of the UI font's signed-distance-field glyph atlas (CoreText, no
// graphics API). Every UI render backend calls it and only uploads the result.

#include <cstdint>

#include "ui.h"

constexpr int kFontFirstChar = 32;
constexpr int kFontCharCount = 96;
constexpr float kFontPixelSize = 28.0f; // logical UI px the atlas is baked at
constexpr int kGlyphPad = 2;            // transparent gutter between atlas cells
constexpr int kSdfSupersample = 4;     // CoreText raster scale before the distance transform
constexpr float kSdfRange = 4.0f;      // logical px the signed distance spans each side of the edge

// Returns malloc'd R8 pixels, top row first; the caller uploads and frees them.
uint8_t *BakeFontAtlas(UiFontMetrics *out, int *outAtlasWidth, int *outAtlasHeight);
