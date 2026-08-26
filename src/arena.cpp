#include "arena.h"

#include <cassert>

Arena ArenaCreate(void *memory, size_t size) {
    Arena arena = {};
    arena.base = (uint8_t *)memory;
    arena.size = size;
    arena.used = 0;
    return arena;
}

void *ArenaPush(Arena *arena, size_t size, size_t alignment) {
    uintptr_t current = (uintptr_t)(arena->base + arena->used);
    uintptr_t aligned = (current + (alignment - 1)) & ~(alignment - 1);
    size_t padding = (size_t)(aligned - current);

    assert(arena->used + padding + size <= arena->size);

    arena->used += padding + size;
    return (void *)aligned;
}
