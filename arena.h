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
    size_t high_water;  // peak usage across all resets
} Arena;

// Initialize the arena with a fixed size
Arena arena_init(size_t size);

// Allocate memory from the arena. Panics on overflow (Memory Safe).
void *arena_alloc(Arena *a, size_t size);

// Reset arena to beginning without freeing buffer
void arena_reset(Arena *a);

// Save current position (returns offset that can be passed to arena_restore)
size_t arena_save(Arena *a);

// Restore arena to a previously saved position
void arena_restore(Arena *a, size_t saved);

// Free arena memory (prints high-watermark stats)
void arena_free(Arena *a);

#endif
