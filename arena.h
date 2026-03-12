#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

// ==========================================
// MEMORY SAFETY: ARENA ALLOCATOR
// ==========================================

typedef struct {
    uint8_t *buffer;
    size_t length;
    size_t capacity;
} Arena;

// Initialize the arena with a fixed size
Arena arena_init(size_t size);

// Allocate memory from the arena. Panics on overflow (Memory Safe).
void *arena_alloc(Arena *a, size_t size);

// Reset arena to beginning without freeing buffer
void arena_reset(Arena *a);

// Free arena memory
void arena_free(Arena *a);

#endif
