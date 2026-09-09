#include "undo_stack.h"

#include <cstring>

#include "arena.h"

namespace {

struct UndoEntry {
    UndoStackFn redo;
    UndoStackFn undo;
    alignas(16) unsigned char payload[kUndoStackPayloadBytes];
};

} // namespace

struct UndoStack {
    void *context;
    UndoEntry entries[kUndoStackCapacity];
    int base;
    int count;  // live entries starting at base
    int cursor; // how many of those are currently applied; redo branch is [cursor, count)
};

UndoStack *UndoStackInit(Arena *arena, void *context) {
    UndoStack *stack = ArenaPushStruct(arena, UndoStack);
    stack->context = context;
    stack->base = 0;
    stack->count = 0;
    stack->cursor = 0;
    return stack;
}

namespace {

int PushSlot(UndoStack *stack) {
    stack->count = stack->cursor; // discard any redo branch
    if (stack->count == kUndoStackCapacity) {
        stack->base = (stack->base + 1) % kUndoStackCapacity;
        --stack->count;
    }
    int slot = (stack->base + stack->count) % kUndoStackCapacity;
    ++stack->count;
    stack->cursor = stack->count;
    return slot;
}

} // namespace

void *UndoStackSubmit(UndoStack *stack, UndoStackFn redo, UndoStackFn undo, const void *payload,
                      size_t payloadBytes) {
    int slot = PushSlot(stack);
    UndoEntry &entry = stack->entries[slot];
    entry.redo = redo;
    entry.undo = undo;
    memset(entry.payload, 0, sizeof entry.payload);
    if (payload != nullptr && payloadBytes > 0) {
        size_t copyBytes = payloadBytes < sizeof entry.payload ? payloadBytes : sizeof entry.payload;
        memcpy(entry.payload, payload, copyBytes);
    }
    if (entry.redo != nullptr) {
        entry.redo(stack->context, entry.payload);
    }
    return entry.payload;
}

void UndoStackUndo(UndoStack *stack) {
    if (stack->cursor == 0) {
        return;
    }
    --stack->cursor;
    UndoEntry &entry = stack->entries[(stack->base + stack->cursor) % kUndoStackCapacity];
    if (entry.undo != nullptr) {
        entry.undo(stack->context, entry.payload);
    }
}

void UndoStackRedo(UndoStack *stack) {
    if (stack->cursor >= stack->count) {
        return;
    }
    UndoEntry &entry = stack->entries[(stack->base + stack->cursor) % kUndoStackCapacity];
    if (entry.redo != nullptr) {
        entry.redo(stack->context, entry.payload);
    }
    ++stack->cursor;
}

bool UndoStackCanUndo(const UndoStack *stack) { return stack->cursor > 0; }
bool UndoStackCanRedo(const UndoStack *stack) { return stack->cursor < stack->count; }

void UndoStackClear(UndoStack *stack) {
    stack->base = 0;
    stack->count = 0;
    stack->cursor = 0;
}
