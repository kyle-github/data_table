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
    return ptr;
}

// Reset arena to beginning without freeing buffer
void arena_reset(Arena *a) {
    a->length = 0;
}

// Free arena memory
void arena_free(Arena *a) {
    free(a->buffer);
    a->length = 0;
    a->capacity = 0;
}
