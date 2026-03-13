#ifndef BYTES_H
#define BYTES_H

#include "arena.h"
#include <stdint.h>
#include <stddef.h>

// ==========================================
// PYTHON ABSTRACTION: BYTES OBJECT
// ==========================================

// Mimics Python's bytes/str object (pointer + length)
typedef struct {
    uint8_t *data;
    size_t len;
} Bytes;

// Helper to wrap a string literal into Bytes
Bytes b_str(Arena *a, const char *str);

// Mimics Python's `b'\x00' * count`
Bytes bytes_repeat(Arena *a, uint8_t byte, size_t count);

// Mimics Python's `struct.pack('<B', val)`
Bytes pack_uint8(Arena *a, uint8_t val);

// Mimics Python's `struct.pack('<H', val)` (Little Endian Short)
Bytes pack_uint16_le(Arena *a, uint16_t val);

// Mimics Python's `struct.pack('<i', val)` (Little Endian Int)
Bytes pack_uint32_le(Arena *a, uint32_t val);

// Mimics Python's `bytes_a + bytes_b`
Bytes bytes_join(Arena *a, Bytes first, Bytes second);

// Helper to concatenate multiple parts to clean up main logic
Bytes bytes_concat(Arena *a, int count, ...);

// ==========================================
// STRUCT PACK/UNPACK - Minimal Python-like API
// ==========================================

// Pack values into bytes according to format string
// Byte order prefix: "<" (little-endian), ">" (big-endian), default native
// Format characters:
//   b = int8, B = uint8 (1 byte)
//   h = int16, H = uint16 (2 bytes)
//   i = int32, I = uint32 (4 bytes)
//   q = int64, Q = uint64 (8 bytes)
//   f = float (4 bytes)
//   d = double (8 bytes)
//   s = C-string (const char*) - packs string bytes, no length prefix or padding
//   x = padding byte (0x00) - can be prefixed with count (e.g., "8x" = 8 zero bytes)
//   * = raw bytes insertion - next arg is Bytes* pointer
// Example: struct_pack(arena, "<BHI", (uint8_t)1, (uint16_t)5, (uint32_t)100)
// Example: struct_pack(arena, "<HH8xI", 1, 2, 100)  // 2 shorts + 8 zero bytes + 1 int
// Example: struct_pack(arena, "<BB*H", 1, 2, &bytes, 5)  // 2 bytes + inserted bytes + 1 short
Bytes struct_pack(Arena *a, const char *fmt, ...);

// Unpack values from bytes according to format string
// Pointers to output variables are passed as variadic arguments
// Returns Bytes of remaining data after parsing, or {NULL, 0} on error
// Format characters (see struct_pack for details)
// For 's' format: pass char buffer pointer AND size_t buffer_size
// Example: uint16_t h; uint32_t i; Bytes rest = struct_unpack(data, "<HI", &h, &i);
Bytes struct_unpack(Bytes data, const char *fmt, ...);

// ==========================================
// BYTES SLICING
// ==========================================

// Create a Bytes from a pointer and length (no allocation, just wraps)
Bytes bytes_from_buf(const uint8_t *buf, size_t len);

// Create a Bytes slice from another Bytes (no allocation, just wraps)
Bytes bytes_slice(Bytes b, size_t offset, size_t len);

// Pad Bytes to even length (adds 0x00 byte if odd length)
// Returns original Bytes if already even, or new Bytes with padding if odd
Bytes bytes_pad_even(Arena *a, Bytes b);

// ==========================================
// DEBUGGING / HEX DUMP
// ==========================================

// Hex dump a Bytes object as 16 columns of hex bytes (no ASCII)
// Each byte is two uppercase hex digits, space-separated
// Example output:
//   54 02 20 06 24 01 0A 0E 13 CD 00 80 9E BE 3F 80
//   35 8A 34 12 01 00 00 00 03 00 00 00 40 13 20 00
void bytes_hexdump(Bytes b, const char *label);

#endif
