#include "arena.h"
#include <stdio.h>
#include <stdlib.h>

// Initialize the arena with a fixed size
Arena arena_init(size_t size) {
    Arena a;
    a.buffer = (uint8_t *)malloc(size);
    if(!a.buffer) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        exit(1);
    }
    a.length = 0;
    a.capacity = size;
    a.high_water = 0;
    return a;
}

// Allocate memory from the arena. Panics on overflow (Memory Safe).
void *arena_alloc(Arena *a, size_t size) {
    if(a->length + size > a->capacity) {
        fprintf(stderr, "Fatal: Arena Out of Memory (Requested %zu, Remaining %zu)\n", size, a->capacity - a->length);
        exit(1);
    }
    void *ptr = a->buffer + a->length;
    a->length += size;
    if(a->length > a->high_water) { a->high_water = a->length; }
    return ptr;
}

// Reset arena to beginning without freeing buffer
void arena_reset(Arena *a) { a->length = 0; }

// Save current position
size_t arena_save(Arena *a) { return a->length; }

// Restore arena to a previously saved position
void arena_restore(Arena *a, size_t saved) { a->length = saved; }

// Free arena memory (prints high-watermark stats)
void arena_free(Arena *a) {
    fprintf(stderr, "[arena] capacity: %zu, high-water: %zu (%.1f%% used)\n", a->capacity, a->high_water,
            a->capacity > 0 ? (double)a->high_water / (double)a->capacity * 100.0 : 0.0);
    free(a->buffer);
    a->length = 0;
    a->capacity = 0;
    a->high_water = 0;
}
