#pragma once

// A fixed-capacity linear undo/redo history, shared by every editor module and
// owned by none of them. A command is two callbacks over an opaque payload
// that the stack stores by value, so no module keeps its own parallel ring.
//
// `context` is passed unchanged to every callback: the editor hands in its
// root state (the SceneState) and each handler casts it to whatever it edits.
// A handler that needs more (the timeline handlers need their TimelineState)
// puts a pointer to it inside the payload.
//
// UndoStackSubmit runs `redo` once and pushes, discarding any redo branch
// first. When the ring is full the oldest entry is dropped (its payload is
// simply forgotten). UndoStackUndo / UndoStackRedo replay the callbacks.

#include <cstddef>

struct Arena;
struct UndoStack;

typedef void (*UndoStackFn)(void *context, void *payload);

constexpr int kUndoStackCapacity = 1024;
constexpr size_t kUndoStackPayloadBytes = 320;

UndoStack *UndoStackInit(Arena *arena, void *context);

// Copies `payloadBytes` (clamped to kUndoStackPayloadBytes) from `payload` into
// the new ring slot, then calls `redo` with a pointer to that stored copy.
// Returns that stored-copy pointer, so a caller can read back a value the redo
// wrote into the payload (it stays valid until the slot is evicted or reused).
void *UndoStackSubmit(UndoStack *stack, UndoStackFn redo, UndoStackFn undo,
                      const void *payload, size_t payloadBytes);

void UndoStackUndo(UndoStack *stack);
void UndoStackRedo(UndoStack *stack);
bool UndoStackCanUndo(const UndoStack *stack);
bool UndoStackCanRedo(const UndoStack *stack);

// Drops the whole history (for a full-scene replacement).
void UndoStackClear(UndoStack *stack);
