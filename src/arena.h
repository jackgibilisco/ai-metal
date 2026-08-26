#pragma once

#include <cstddef>
#include <cstdint>

struct Arena {
    uint8_t *base;
    size_t size;
    size_t used;
};

Arena ArenaCreate(void *memory, size_t size);
void *ArenaPush(Arena *arena, size_t size, size_t alignment);

#define ArenaPushStruct(arena, type) (type *)ArenaPush(arena, sizeof(type), alignof(type))
#define ArenaPushArray(arena, type, count) (type *)ArenaPush(arena, sizeof(type) * (count), alignof(type))
