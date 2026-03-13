#include "bytes.h"
#include "arena.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s\n", msg); \
        return 0; \
    } \
} while(0)

#define TEST_PASS() do { printf("  PASS\n"); return 1; } while(0)

void print_bytes(const char *label, Bytes b) {
    printf("  %s (%zu bytes): ", label, b.len);
    for(size_t i = 0; i < b.len; i++) {
        printf("%02X ", b.data[i]);
    }
    printf("\n");
}

// Test b_str
int test_b_str(Arena *a) {
    printf("Test: b_str\n");
    Bytes result = b_str(a, "Hello");
    TEST_ASSERT(result.len == 5, "Length should be 5");
    TEST_ASSERT(memcmp(result.data, "Hello", 5) == 0, "Content should match");
    TEST_PASS();
}

// Test bytes_repeat
int test_bytes_repeat(Arena *a) {
    printf("Test: bytes_repeat\n");
    Bytes result = bytes_repeat(a, 0xAB, 5);
    TEST_ASSERT(result.len == 5, "Length should be 5");
    for(size_t i = 0; i < 5; i++) {
        TEST_ASSERT(result.data[i] == 0xAB, "All bytes should be 0xAB");
    }
    TEST_PASS();
}

// Test pack_uint8
int test_pack_uint8(Arena *a) {
    printf("Test: pack_uint8\n");
    Bytes result = pack_uint8(a, 0x42);
    TEST_ASSERT(result.len == 1, "Length should be 1");
    TEST_ASSERT(result.data[0] == 0x42, "Value should be 0x42");
    TEST_PASS();
}

// Test pack_uint16_le
int test_pack_uint16_le(Arena *a) {
    printf("Test: pack_uint16_le\n");
    Bytes result = pack_uint16_le(a, 0x1234);
    TEST_ASSERT(result.len == 2, "Length should be 2");
    TEST_ASSERT(result.data[0] == 0x34, "Low byte should be 0x34");
    TEST_ASSERT(result.data[1] == 0x12, "High byte should be 0x12");
    TEST_PASS();
}

// Test pack_uint32_le
int test_pack_uint32_le(Arena *a) {
    printf("Test: pack_uint32_le\n");
    Bytes result = pack_uint32_le(a, 0x12345678);
    TEST_ASSERT(result.len == 4, "Length should be 4");
    TEST_ASSERT(result.data[0] == 0x78, "Byte 0 should be 0x78");
    TEST_ASSERT(result.data[1] == 0x56, "Byte 1 should be 0x56");
    TEST_ASSERT(result.data[2] == 0x34, "Byte 2 should be 0x34");
    TEST_ASSERT(result.data[3] == 0x12, "Byte 3 should be 0x12");
    TEST_PASS();
}

// Test bytes_join
int test_bytes_join(Arena *a) {
    printf("Test: bytes_join\n");
    Bytes first = struct_pack(a, "<HH", 0x1234, 0x5678);
    Bytes second = struct_pack(a, "<I", 0xABCDEF00);
    Bytes result = bytes_join(a, first, second);
    TEST_ASSERT(result.len == 8, "Length should be 8");
    TEST_ASSERT(result.data[0] == 0x34, "First byte correct");
    TEST_ASSERT(result.data[7] == 0xAB, "Last byte correct");
    TEST_PASS();
}

// Test bytes_concat
int test_bytes_concat(Arena *a) {
    printf("Test: bytes_concat\n");
    Bytes b1 = struct_pack(a, "<B", 0x11);
    Bytes b2 = struct_pack(a, "<H", 0x2233);
    Bytes b3 = struct_pack(a, "<I", 0x44556677);
    Bytes result = bytes_concat(a, 3, b1, b2, b3);
    TEST_ASSERT(result.len == 7, "Length should be 7");
    TEST_ASSERT(result.data[0] == 0x11, "First byte correct");
    TEST_ASSERT(result.data[1] == 0x33, "Second byte correct");
    TEST_ASSERT(result.data[6] == 0x44, "Last byte correct");
    TEST_PASS();
}

// Test bytes_from_buf
int test_bytes_from_buf(Arena *a) {
    printf("Test: bytes_from_buf\n");
    uint8_t buffer[] = {0xAA, 0xBB, 0xCC, 0xDD};
    Bytes result = bytes_from_buf(buffer, 4);
    TEST_ASSERT(result.len == 4, "Length should be 4");
    TEST_ASSERT(result.data == buffer, "Should point to same buffer");
    TEST_ASSERT(result.data[0] == 0xAA, "First byte correct");
    TEST_PASS();
}

// Test bytes_slice
int test_bytes_slice(Arena *a) {
    printf("Test: bytes_slice\n");
    Bytes original = struct_pack(a, "<BBBBBB", 0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
    Bytes slice = bytes_slice(original, 2, 3);
    TEST_ASSERT(slice.len == 3, "Length should be 3");
    TEST_ASSERT(slice.data[0] == 0x33, "First byte should be 0x33");
    TEST_ASSERT(slice.data[1] == 0x44, "Second byte should be 0x44");
    TEST_ASSERT(slice.data[2] == 0x55, "Third byte should be 0x55");
    TEST_PASS();
}

// Test bytes_pad_even - even length
int test_bytes_pad_even_even(Arena *a) {
    printf("Test: bytes_pad_even (even length)\n");
    Bytes even = struct_pack(a, "<HH", 0x1234, 0x5678);
    Bytes padded = bytes_pad_even(a, even);
    TEST_ASSERT(padded.len == 4, "Length should remain 4");
    TEST_ASSERT(padded.data == even.data, "Should be same buffer");
    TEST_PASS();
}

// Test bytes_pad_even - odd length
int test_bytes_pad_even_odd(Arena *a) {
    printf("Test: bytes_pad_even (odd length)\n");
    Bytes odd = struct_pack(a, "<BHH", 0x11, 0x2233, 0x4455);
    print_bytes("Original", odd);
    Bytes padded = bytes_pad_even(a, odd);
    print_bytes("Padded", padded);
    TEST_ASSERT(padded.len == 6, "Length should be 6");
    // Original data: 0x11, 0x33, 0x22, 0x55, 0x44 (5 bytes in little-endian)
    TEST_ASSERT(padded.data[4] == 0x44, "Second to last byte correct");
    TEST_ASSERT(padded.data[5] == 0x00, "Padding byte should be 0x00");
    TEST_PASS();
}

// Test struct_pack - all integer types little-endian
int test_struct_pack_integers_le(Arena *a) {
    printf("Test: struct_pack (integers little-endian)\n");
    Bytes result = struct_pack(a, "<bBhHiIqQ",
                              (int8_t)-1,
                              (uint8_t)0xFF,
                              (int16_t)-256,
                              (uint16_t)0x1234,
                              (int32_t)-65536,
                              (uint32_t)0x12345678,
                              (int64_t)-1LL,
                              (uint64_t)0x123456789ABCDEF0ULL);
    TEST_ASSERT(result.len == 30, "Length should be 30");
    // Check a few key bytes
    TEST_ASSERT(result.data[0] == 0xFF, "int8_t -1 should be 0xFF");
    TEST_ASSERT(result.data[1] == 0xFF, "uint8_t 0xFF correct");
    TEST_ASSERT(result.data[4] == 0x34 && result.data[5] == 0x12, "uint16_t little-endian");
    TEST_PASS();
}

// Test struct_pack - big-endian
int test_struct_pack_big_endian(Arena *a) {
    printf("Test: struct_pack (big-endian)\n");
    Bytes result = struct_pack(a, ">HI", (uint16_t)0x1234, (uint32_t)0x56789ABC);
    TEST_ASSERT(result.len == 6, "Length should be 6");
    TEST_ASSERT(result.data[0] == 0x12, "Big-endian high byte first");
    TEST_ASSERT(result.data[1] == 0x34, "Big-endian low byte second");
    TEST_ASSERT(result.data[2] == 0x56, "Big-endian first byte");
    TEST_ASSERT(result.data[5] == 0xBC, "Big-endian last byte");
    TEST_PASS();
}

// Test struct_pack - float and double
int test_struct_pack_floats(Arena *a) {
    printf("Test: struct_pack (float and double)\n");
    float f = 3.14159f;
    double d = 2.71828;
    Bytes result = struct_pack(a, "<fd", f, d);
    TEST_ASSERT(result.len == 12, "Length should be 12 (4 + 8)");

    // Verify by unpacking - check byte patterns
    uint32_t f_bits_in, f_bits_out;
    uint64_t d_bits_in, d_bits_out;

    memcpy(&f_bits_in, &f, 4);
    memcpy(&d_bits_in, &d, 8);
    memcpy(&f_bits_out, result.data, 4);
    memcpy(&d_bits_out, result.data + 4, 8);

    if (f_bits_out != f_bits_in) {
        printf("  Float: expected 0x%08X, got 0x%08X\n", f_bits_in, f_bits_out);
    }
    if (d_bits_out != d_bits_in) {
        printf("  Double: expected 0x%016llX, got 0x%016llX\n",
               (unsigned long long)d_bits_in, (unsigned long long)d_bits_out);
    }

    TEST_ASSERT(f_bits_out == f_bits_in, "Float bit pattern preserved");
    TEST_ASSERT(d_bits_out == d_bits_in, "Double bit pattern preserved");
    TEST_PASS();
}

// Test struct_pack - string
int test_struct_pack_string(Arena *a) {
    printf("Test: struct_pack (string)\n");
    Bytes result = struct_pack(a, "<Bs", (uint8_t)0xAA, "Hello");
    TEST_ASSERT(result.len == 6, "Length should be 6 (1 + 5)");
    TEST_ASSERT(result.data[0] == 0xAA, "First byte correct");
    TEST_ASSERT(memcmp(result.data + 1, "Hello", 5) == 0, "String content correct");
    TEST_PASS();
}

// Test struct_pack - padding with x
int test_struct_pack_padding(Arena *a) {
    printf("Test: struct_pack (padding)\n");
    Bytes result = struct_pack(a, "<HH8xI", (uint16_t)0x1234, (uint16_t)0x5678, (uint32_t)0xABCDEF00);
    TEST_ASSERT(result.len == 16, "Length should be 16");
    TEST_ASSERT(result.data[0] == 0x34, "First byte correct");
    // Check padding bytes
    for(size_t i = 4; i < 12; i++) {
        TEST_ASSERT(result.data[i] == 0x00, "Padding byte should be 0x00");
    }
    TEST_ASSERT(result.data[12] == 0x00 && result.data[15] == 0xAB, "Last int correct");
    TEST_PASS();
}

// Test struct_pack - raw bytes insertion
int test_struct_pack_raw_bytes(Arena *a) {
    printf("Test: struct_pack (raw bytes insertion)\n");
    Bytes inner = struct_pack(a, "<HH", (uint16_t)0xAAAA, (uint16_t)0xBBBB);
    Bytes result = struct_pack(a, "<BB*I", (uint8_t)0x11, (uint8_t)0x22, &inner, (uint32_t)0xCCCCCCCC);
    TEST_ASSERT(result.len == 10, "Length should be 10");
    TEST_ASSERT(result.data[0] == 0x11, "First byte correct");
    TEST_ASSERT(result.data[1] == 0x22, "Second byte correct");
    TEST_ASSERT(result.data[2] == 0xAA && result.data[3] == 0xAA, "Inserted bytes correct");
    TEST_ASSERT(result.data[9] == 0xCC, "Last byte correct");
    TEST_PASS();
}

// Test struct_unpack - basic types
int test_struct_unpack_basic(Arena *a) {
    printf("Test: struct_unpack (basic types)\n");
    Bytes data = struct_pack(a, "<BHI", (uint8_t)0x42, (uint16_t)0x1234, (uint32_t)0x56789ABC);

    uint8_t b = 0;
    uint16_t h = 0;
    uint32_t i = 0;
    Bytes remaining = struct_unpack(data, "<BHI", &b, &h, &i);

    TEST_ASSERT(b == 0x42, "uint8_t unpacked correctly");
    TEST_ASSERT(h == 0x1234, "uint16_t unpacked correctly");
    TEST_ASSERT(i == 0x56789ABC, "uint32_t unpacked correctly");
    TEST_ASSERT(remaining.len == 0, "No remaining bytes");
    TEST_PASS();
}

// Test struct_unpack - signed types
int test_struct_unpack_signed(Arena *a) {
    printf("Test: struct_unpack (signed types)\n");
    Bytes data = struct_pack(a, "<bhi", (int8_t)-10, (int16_t)-1000, (int32_t)-100000);

    int8_t b = 0;
    int16_t h = 0;
    int32_t i = 0;
    Bytes remaining = struct_unpack(data, "<bhi", &b, &h, &i);

    TEST_ASSERT(b == -10, "int8_t unpacked correctly");
    TEST_ASSERT(h == -1000, "int16_t unpacked correctly");
    TEST_ASSERT(i == -100000, "int32_t unpacked correctly");
    TEST_ASSERT(remaining.len == 0, "No remaining bytes");
    TEST_PASS();
}

// Test struct_unpack - 64-bit types
int test_struct_unpack_64bit(Arena *a) {
    printf("Test: struct_unpack (64-bit types)\n");
    Bytes data = struct_pack(a, "<qQ", (int64_t)-123456789LL, (uint64_t)0x123456789ABCDEF0ULL);

    int64_t q = 0;
    uint64_t Q = 0;
    Bytes remaining = struct_unpack(data, "<qQ", &q, &Q);

    TEST_ASSERT(q == -123456789LL, "int64_t unpacked correctly");
    TEST_ASSERT(Q == 0x123456789ABCDEF0ULL, "uint64_t unpacked correctly");
    TEST_ASSERT(remaining.len == 0, "No remaining bytes");
    TEST_PASS();
}

// Test struct_unpack - floats
int test_struct_unpack_floats(Arena *a) {
    printf("Test: struct_unpack (floats)\n");
    float f_in = 3.14159f;
    double d_in = 2.71828;
    Bytes data = struct_pack(a, "<fd", f_in, d_in);

    float f_out = 0.0f;
    double d_out = 0.0;
    Bytes remaining = struct_unpack(data, "<fd", &f_out, &d_out);

    // Compare bit patterns to avoid floating point precision issues
    uint32_t f_bits_in, f_bits_out;
    uint64_t d_bits_in, d_bits_out;
    memcpy(&f_bits_in, &f_in, 4);
    memcpy(&f_bits_out, &f_out, 4);
    memcpy(&d_bits_in, &d_in, 8);
    memcpy(&d_bits_out, &d_out, 8);

    if (f_bits_out != f_bits_in) {
        printf("  Float unpack: expected 0x%08X, got 0x%08X\n", f_bits_in, f_bits_out);
        printf("  Float values: in=%.6f, out=%.6f\n", f_in, f_out);
    }
    if (d_bits_out != d_bits_in) {
        printf("  Double unpack: expected 0x%016llX, got 0x%016llX\n",
               (unsigned long long)d_bits_in, (unsigned long long)d_bits_out);
    }

    TEST_ASSERT(f_bits_out == f_bits_in, "float unpacked correctly");
    TEST_ASSERT(d_bits_out == d_bits_in, "double unpacked correctly");
    TEST_ASSERT(remaining.len == 0, "No remaining bytes");
    TEST_PASS();
}

// Test struct_unpack - big-endian
int test_struct_unpack_big_endian(Arena *a) {
    printf("Test: struct_unpack (big-endian)\n");
    Bytes data = struct_pack(a, ">HI", (uint16_t)0x1234, (uint32_t)0x56789ABC);

    uint16_t h = 0;
    uint32_t i = 0;
    Bytes remaining = struct_unpack(data, ">HI", &h, &i);

    TEST_ASSERT(h == 0x1234, "uint16_t big-endian unpacked");
    TEST_ASSERT(i == 0x56789ABC, "uint32_t big-endian unpacked");
    TEST_ASSERT(remaining.len == 0, "No remaining bytes");
    TEST_PASS();
}

// Test struct_unpack - remaining bytes
int test_struct_unpack_remaining(Arena *a) {
    printf("Test: struct_unpack (remaining bytes)\n");
    Bytes data = struct_pack(a, "<BHHHH", (uint8_t)0xAA, (uint16_t)0x1234,
                            (uint16_t)0x5678, (uint16_t)0x9ABC, (uint16_t)0xDEF0);

    uint8_t b = 0;
    uint16_t h1 = 0;
    // Only unpack first 2 fields
    Bytes remaining = struct_unpack(data, "<BH", &b, &h1);

    TEST_ASSERT(b == 0xAA, "First byte unpacked");
    TEST_ASSERT(h1 == 0x1234, "First uint16 unpacked");
    TEST_ASSERT(remaining.len == 6, "Should have 6 bytes remaining");

    // Unpack remaining data
    uint16_t h2 = 0, h3 = 0, h4 = 0;
    Bytes final = struct_unpack(remaining, "<HHH", &h2, &h3, &h4);
    TEST_ASSERT(h2 == 0x5678, "Second uint16 unpacked");
    TEST_ASSERT(h3 == 0x9ABC, "Third uint16 unpacked");
    TEST_ASSERT(h4 == 0xDEF0, "Fourth uint16 unpacked");
    TEST_ASSERT(final.len == 0, "All data consumed");
    TEST_PASS();
}

// Test struct_unpack - error handling (insufficient data)
int test_struct_unpack_error(Arena *a) {
    printf("Test: struct_unpack (error - insufficient data)\n");
    Bytes data = struct_pack(a, "<BH", (uint8_t)0x11, (uint16_t)0x2233);

    uint8_t b = 0;
    uint16_t h = 0;
    uint32_t i = 0;
    // Try to unpack more than available
    Bytes remaining = struct_unpack(data, "<BHI", &b, &h, &i);

    TEST_ASSERT(remaining.data == NULL, "Should return NULL on error");
    TEST_ASSERT(remaining.len == 0, "Should return 0 length on error");
    TEST_PASS();
}

// Test round-trip pack/unpack
int test_pack_unpack_roundtrip(Arena *a) {
    printf("Test: pack/unpack round-trip\n");
    uint8_t b_in = 0x12;
    uint16_t h_in = 0x3456;
    uint32_t i_in = 0x789ABCDE;
    uint64_t q_in = 0xFEDCBA9876543210ULL;

    Bytes packed = struct_pack(a, "<BHIQ", b_in, h_in, i_in, q_in);

    uint8_t b_out = 0;
    uint16_t h_out = 0;
    uint32_t i_out = 0;
    uint64_t q_out = 0;

    struct_unpack(packed, "<BHIQ", &b_out, &h_out, &i_out, &q_out);

    TEST_ASSERT(b_out == b_in, "uint8_t round-trip");
    TEST_ASSERT(h_out == h_in, "uint16_t round-trip");
    TEST_ASSERT(i_out == i_in, "uint32_t round-trip");
    TEST_ASSERT(q_out == q_in, "uint64_t round-trip");
    TEST_PASS();
}

int main() {
    Arena a = arena_init(16 * 1024);  // 16KB for tests
    int passed = 0;
    int total = 0;

    #define RUN_TEST(test) do { \
        total++; \
        arena_reset(&a); \
        if (test(&a)) passed++; \
    } while(0)

    printf("========================================\n");
    printf("Running bytes.h Test Suite\n");
    printf("========================================\n\n");

    // Basic helper functions
    RUN_TEST(test_b_str);
    RUN_TEST(test_bytes_repeat);
    RUN_TEST(test_pack_uint8);
    RUN_TEST(test_pack_uint16_le);
    RUN_TEST(test_pack_uint32_le);

    // Bytes manipulation
    RUN_TEST(test_bytes_join);
    RUN_TEST(test_bytes_concat);
    RUN_TEST(test_bytes_from_buf);
    RUN_TEST(test_bytes_slice);
    RUN_TEST(test_bytes_pad_even_even);
    RUN_TEST(test_bytes_pad_even_odd);

    // struct_pack tests
    RUN_TEST(test_struct_pack_integers_le);
    RUN_TEST(test_struct_pack_big_endian);
    RUN_TEST(test_struct_pack_floats);
    RUN_TEST(test_struct_pack_string);
    RUN_TEST(test_struct_pack_padding);
    RUN_TEST(test_struct_pack_raw_bytes);

    // struct_unpack tests
    RUN_TEST(test_struct_unpack_basic);
    RUN_TEST(test_struct_unpack_signed);
    RUN_TEST(test_struct_unpack_64bit);
    RUN_TEST(test_struct_unpack_floats);
    RUN_TEST(test_struct_unpack_big_endian);
    RUN_TEST(test_struct_unpack_remaining);
    RUN_TEST(test_struct_unpack_error);

    // Integration tests
    RUN_TEST(test_pack_unpack_roundtrip);

    printf("\n========================================\n");
    printf("Test Results: %d/%d passed\n", passed, total);
    printf("========================================\n");

    arena_free(&a);
    return (passed == total) ? 0 : 1;
}
