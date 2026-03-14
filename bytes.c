#include "bytes.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Helper to wrap a string literal into Bytes
Bytes b_str(Arena *a, const char *str) {
    size_t len = strlen(str);
    uint8_t *mem = arena_alloc(a, len);
    memcpy(mem, str, len);
    return (Bytes){mem, len};
}

// Mimics Python's `b'\x00' * count`
Bytes bytes_repeat(Arena *a, uint8_t byte, size_t count) {
    uint8_t *mem = arena_alloc(a, count);
    memset(mem, byte, count);
    return (Bytes){mem, count};
}

// Mimics Python's `struct.pack('<B', val)`
Bytes pack_uint8(Arena *a, uint8_t val) {
    uint8_t *mem = arena_alloc(a, 1);
    mem[0] = val;
    return (Bytes){mem, 1};
}

// Mimics Python's `struct.pack('<H', val)` (Little Endian Short)
Bytes pack_uint16_le(Arena *a, uint16_t val) {
    uint8_t *mem = arena_alloc(a, 2);
    mem[0] = val & 0xFF;
    mem[1] = (val >> 8) & 0xFF;
    return (Bytes){mem, 2};
}

// Mimics Python's `struct.pack('<i', val)` (Little Endian Int)
Bytes pack_uint32_le(Arena *a, uint32_t val) {
    uint8_t *mem = arena_alloc(a, 4);
    mem[0] = val & 0xFF;
    mem[1] = (val >> 8) & 0xFF;
    mem[2] = (val >> 16) & 0xFF;
    mem[3] = (val >> 24) & 0xFF;
    return (Bytes){mem, 4};
}

// Mimics Python's `bytes_a + bytes_b`
Bytes bytes_join(Arena *a, Bytes first, Bytes second) {
    size_t new_len = first.len + second.len;
    uint8_t *mem = arena_alloc(a, new_len);

    memcpy(mem, first.data, first.len);
    memcpy(mem + first.len, second.data, second.len);

    return (Bytes){mem, new_len};
}

// Helper to concatenate multiple parts to clean up main logic
Bytes bytes_concat(Arena *a, int count, ...) {
    va_list args;
    size_t total_len = 0;

    // Pass 1: Calculate Size
    va_start(args, count);
    for(int i = 0; i < count; i++) {
        Bytes b = va_arg(args, Bytes);
        total_len += b.len;
    }
    va_end(args);

    uint8_t *mem = arena_alloc(a, total_len);
    size_t offset = 0;

    // Pass 2: Copy
    va_start(args, count);
    for(int i = 0; i < count; i++) {
        Bytes b = va_arg(args, Bytes);
        memcpy(mem + offset, b.data, b.len);
        offset += b.len;
    }
    va_end(args);

    return (Bytes){mem, total_len};
}

// ==========================================
// STRUCT PACK/UNPACK IMPLEMENTATION
// ==========================================

// Determine byte order from format string
// Returns 1 for little-endian, -1 for big-endian, 0 for native
static int get_byte_order(const char *fmt) {
    if(!fmt || !*fmt) {
        return 0;  // native
    }
    if(*fmt == '<') {
        return 1;  // little-endian
    }
    if(*fmt == '>') {
        return -1;  // big-endian
    }
    if(*fmt == '=') {
        return 0;  // native
    }
    if(*fmt == '@') {
        return 0;  // native
    }
    return 0;  // default: native
}

// Skip byte order prefix if present
static const char *skip_byte_order(const char *fmt) {
    if(!fmt || !*fmt) { return fmt; }
    if(*fmt == '<' || *fmt == '>' || *fmt == '=' || *fmt == '@') { return fmt + 1; }
    return fmt;
}

// Pack single value into buffer at offset
// For 's' format, value should be cast to (const char*)
// Returns number of bytes written, or 0 on error
static size_t pack_value(uint8_t *buf, uint64_t value, const char *str, char fmt_char, int byte_order) {
    if(byte_order == 0) {
        byte_order = 1;  // default to little-endian
    }

    switch(fmt_char) {
        case 'b':
        case 'B': buf[0] = (uint8_t)value; return 1;

        case 'h':
        case 'H': {
            uint16_t v = (uint16_t)value;
            if(byte_order > 0) {
                buf[0] = v & 0xFF;
                buf[1] = (v >> 8) & 0xFF;
            } else {
                buf[0] = (v >> 8) & 0xFF;
                buf[1] = v & 0xFF;
            }
            return 2;
        }

        case 'i':
        case 'I': {
            uint32_t v = (uint32_t)value;
            if(byte_order > 0) {
                buf[0] = v & 0xFF;
                buf[1] = (v >> 8) & 0xFF;
                buf[2] = (v >> 16) & 0xFF;
                buf[3] = (v >> 24) & 0xFF;
            } else {
                buf[0] = (v >> 24) & 0xFF;
                buf[1] = (v >> 16) & 0xFF;
                buf[2] = (v >> 8) & 0xFF;
                buf[3] = v & 0xFF;
            }
            return 4;
        }

        case 'q':
        case 'Q': {
            uint64_t v = value;
            if(byte_order > 0) {
                for(int i = 0; i < 8; i++) { buf[i] = (v >> (i * 8)) & 0xFF; }
            } else {
                for(int i = 0; i < 8; i++) { buf[i] = (v >> ((7 - i) * 8)) & 0xFF; }
            }
            return 8;
        }

        case 'f': {
            uint32_t v = (uint32_t)value;  // bit representation
            if(byte_order > 0) {
                buf[0] = v & 0xFF;
                buf[1] = (v >> 8) & 0xFF;
                buf[2] = (v >> 16) & 0xFF;
                buf[3] = (v >> 24) & 0xFF;
            } else {
                buf[0] = (v >> 24) & 0xFF;
                buf[1] = (v >> 16) & 0xFF;
                buf[2] = (v >> 8) & 0xFF;
                buf[3] = v & 0xFF;
            }
            return 4;
        }

        case 'd': {
            uint64_t v = value;  // bit representation
            if(byte_order > 0) {
                for(int i = 0; i < 8; i++) { buf[i] = (v >> (i * 8)) & 0xFF; }
            } else {
                for(int i = 0; i < 8; i++) { buf[i] = (v >> ((7 - i) * 8)) & 0xFF; }
            }
            return 8;
        }

        case 's': {
            // C-string: pack raw bytes, no length prefix or padding
            if(!str) { return 0; }
            size_t len = strlen(str);
            memcpy(buf, str, len);
            return len;
        }

        default: return 0;
    }
}

// Unpack single value from buffer at offset
// Returns the value as uint64_t (or bit-compatible representation for float/double)
static uint64_t unpack_value(const uint8_t *buf, char fmt_char, int byte_order) {
    if(byte_order == 0) {
        byte_order = 1;  // default to little-endian
    }

    switch(fmt_char) {
        case 'b':
        case 'B': return (uint64_t)buf[0];

        case 'h':
        case 'H': {
            if(byte_order > 0) {
                return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8);
            } else {
                return ((uint64_t)buf[0] << 8) | (uint64_t)buf[1];
            }
        }

        case 'i':
        case 'I': {
            if(byte_order > 0) {
                return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24);
            } else {
                return ((uint64_t)buf[0] << 24) | ((uint64_t)buf[1] << 16) | ((uint64_t)buf[2] << 8) | (uint64_t)buf[3];
            }
        }

        case 'q':
        case 'Q': {
            uint64_t v = 0;
            if(byte_order > 0) {
                for(int i = 0; i < 8; i++) { v |= ((uint64_t)buf[i] << (i * 8)); }
            } else {
                for(int i = 0; i < 8; i++) { v |= ((uint64_t)buf[i] << ((7 - i) * 8)); }
            }
            return v;
        }

        case 'f':
        case 'd': {
            uint64_t v = 0;
            if(byte_order > 0) {
                for(int i = 0; i < (fmt_char == 'f' ? 4 : 8); i++) { v |= ((uint64_t)buf[i] << (i * 8)); }
            } else {
                int size = (fmt_char == 'f' ? 4 : 8);
                for(int i = 0; i < size; i++) { v |= ((uint64_t)buf[i] << ((size - 1 - i) * 8)); }
            }
            return v;
        }

        case 's':
            // String unpack: handled separately in bytes_unpack
            return 0;

        default: return 0;
    }
}

// Pack values into bytes according to format string
Bytes bytes_pack(Arena *a, const char *fmt, ...) {
    if(!fmt || !*fmt) { return (Bytes){NULL, 0}; }

    int byte_order = get_byte_order(fmt);
    const char *format_chars = skip_byte_order(fmt);

    // First pass: calculate total size needed
    va_list args_count;
    va_start(args_count, fmt);
    size_t total_size = 0;
    for(const char *p = format_chars; *p;) {
        // Parse optional count prefix
        size_t count = 1;
        if(*p >= '0' && *p <= '9') {
            count = 0;
            while(*p >= '0' && *p <= '9') {
                count = count * 10 + (*p - '0');
                p++;
            }
            if(!*p) {
                break;  // malformed format
            }
        }

        char c = *p++;
        if(c == 'x') {
            // Padding bytes - no args consumed
            total_size += count;
        } else if(c == '*') {
            // Raw bytes insertion - consume Bytes* arg
            Bytes *b = va_arg(args_count, Bytes *);
            if(b) { total_size += b->len; }
        } else if(c == 's') {
            // String - consume const char* arg
            const char *str = va_arg(args_count, const char *);
            if(str) { total_size += strlen(str); }
        } else if(c == 'b' || c == 'B') {
            total_size += 1 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'h' || c == 'H') {
            total_size += 2 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'i' || c == 'I') {
            total_size += 4 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'f') {
            total_size += 4 * count;
            for(size_t i = 0; i < count; i++) {
                va_arg(args_count, double);  // floats promoted to double
            }
        } else if(c == 'q' || c == 'Q') {
            total_size += 8 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'd') {
            total_size += 8 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, double); }
        } else {
            // Unknown format, consume arg
            va_arg(args_count, uint64_t);
        }
    }
    va_end(args_count);

    if(total_size == 0) { return (Bytes){NULL, 0}; }

    // Allocate buffer
    uint8_t *buf = arena_alloc(a, total_size);

    // Second pass: pack values
    va_list args;
    va_start(args, fmt);

    size_t offset = 0;
    for(const char *p = format_chars; *p;) {
        // Parse optional count prefix
        size_t count = 1;
        if(*p >= '0' && *p <= '9') {
            count = 0;
            while(*p >= '0' && *p <= '9') {
                count = count * 10 + (*p - '0');
                p++;
            }
            if(!*p) {
                break;  // malformed format
            }
        }

        char c = *p++;
        if(c == 'x') {
            // Padding bytes
            memset(buf + offset, 0, count);
            offset += count;
        } else if(c == '*') {
            // Raw bytes insertion
            Bytes *b = va_arg(args, Bytes *);
            if(b && b->data) {
                memcpy(buf + offset, b->data, b->len);
                offset += b->len;
            }
        } else if(c == 's') {
            const char *str = va_arg(args, const char *);
            size_t written = pack_value(buf + offset, 0, str, c, byte_order);
            offset += written;
        } else {
            // Regular format characters
            for(size_t i = 0; i < count; i++) {
                uint64_t val;
                if(c == 'f') {
                    // Floats are promoted to double in variadic args
                    double d = va_arg(args, double);
                    float f = (float)d;
                    uint32_t bits;
                    memcpy(&bits, &f, 4);
                    val = bits;
                } else if(c == 'd') {
                    double d = va_arg(args, double);
                    memcpy(&val, &d, 8);
                } else {
                    val = va_arg(args, uint64_t);
                }
                size_t written = pack_value(buf + offset, val, NULL, c, byte_order);
                offset += written;
            }
        }
    }

    va_end(args);

    return (Bytes){buf, offset};
}

// Unpack values from bytes according to format string
Bytes bytes_unpack(Bytes data, const char *fmt, ...) {
    if(!fmt || !*fmt || !data.data) { return (Bytes){NULL, 0}; }

    int byte_order = get_byte_order(fmt);
    const char *format_chars = skip_byte_order(fmt);

    // Calculate expected size (excluding strings)
    size_t expected_size = 0;
    for(const char *p = format_chars; *p; p++) {
        char c = *p;
        if(c == 'b' || c == 'B') {
            expected_size += 1;
        } else if(c == 'h' || c == 'H') {
            expected_size += 2;
        } else if(c == 'i' || c == 'I' || c == 'f') {
            expected_size += 4;
        } else if(c == 'q' || c == 'Q' || c == 'd') {
            expected_size += 8;
        }
    }

    if(data.len < expected_size) {
        return (Bytes){NULL, 0};  // not enough data
    }

    // Unpack values
    va_list args;
    va_start(args, fmt);

    size_t offset = 0;
    for(const char *p = format_chars; *p; p++) {
        char c = *p;
        void *ptr = va_arg(args, void *);
        uint64_t val = unpack_value(data.data + offset, c, byte_order);

        // Store result in appropriate type
        if(c == 'b') {
            *(int8_t *)ptr = (int8_t)val;
            offset += 1;
        } else if(c == 'B') {
            *(uint8_t *)ptr = (uint8_t)val;
            offset += 1;
        } else if(c == 'h') {
            *(int16_t *)ptr = (int16_t)val;
            offset += 2;
        } else if(c == 'H') {
            *(uint16_t *)ptr = (uint16_t)val;
            offset += 2;
        } else if(c == 'i') {
            *(int32_t *)ptr = (int32_t)val;
            offset += 4;
        } else if(c == 'I') {
            *(uint32_t *)ptr = (uint32_t)val;
            offset += 4;
        } else if(c == 'q') {
            *(int64_t *)ptr = (int64_t)val;
            offset += 8;
        } else if(c == 'Q') {
            *(uint64_t *)ptr = val;
            offset += 8;
        } else if(c == 'f') {
            float fval = *(float *)&val;
            *(float *)ptr = fval;
            offset += 4;
        } else if(c == 'd') {
            double dval = *(double *)&val;
            *(double *)ptr = dval;
            offset += 8;
        }
    }

    va_end(args);

    return bytes_slice(data, offset, data.len - offset);
}

// ==========================================
// BYTES SLICING IMPLEMENTATION
// ==========================================

// Create a Bytes from a pointer and length (no allocation, just wraps)
Bytes bytes_from_buf(const uint8_t *buf, size_t len) { return (Bytes){(uint8_t *)buf, len}; }

// Create a Bytes slice from another Bytes (no allocation, just wraps)
Bytes bytes_slice(Bytes b, size_t offset, size_t len) {
    if(offset + len > b.len) {
        return (Bytes){NULL, 0};  // invalid slice
    }
    return (Bytes){b.data + offset, len};
}

// Pad Bytes to even length (adds 0x00 byte if odd length)
Bytes bytes_pad_even(Arena *a, Bytes b) {
    if(b.len % 2 == 0) {
        return b;  // Already even
    }
    // Odd length: add padding byte
    return bytes_concat(a, 2, b, pack_uint8(a, 0x00));
}

// ==========================================
// DEBUGGING / HEX DUMP
// ==========================================

void bytes_hexdump(Bytes b, const char *label) {
    printf("\n%s (%zu bytes):\n", label, b.len);
    for(size_t i = 0; i < b.len; i += 16) {
        printf("  ");
        size_t chunk_size = (b.len - i < 16) ? (b.len - i) : 16;
        for(size_t j = 0; j < chunk_size; j++) {
            printf("%02X", b.data[i + j]);
            if(j < chunk_size - 1) { printf(" "); }
        }
        printf("\n");
    }
}
