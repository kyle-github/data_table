#include "bytes.h"
#include "arena.h"
#include <stdio.h>
#include <string.h>

void print_bytes(const char *label, Bytes b) {
    printf("%s (%zu bytes): ", label, b.len);
    for(size_t i = 0; i < b.len; i++) {
        printf("%02X ", b.data[i]);
    }
    printf("\n");
}

int main() {
    Arena a = arena_init(1024);

    // Test 1: Padding with 'x'
    printf("Test 1: Padding with 'x'\n");
    Bytes test1 = struct_pack(&a, "<HH8xI", 0x1234, 0x5678, 0xABCDEF00);
    print_bytes("  Expected: 34 12 78 56 [8 zeros] 00 EF CD AB", test1);
    printf("  Expected length: 16, got: %zu\n\n", test1.len);
    arena_reset(&a);

    // Test 2: Raw bytes insertion with '*'
    printf("Test 2: Raw bytes insertion with '*'\n");
    Bytes inner = struct_pack(&a, "<HH", 0xAAAA, 0xBBBB);
    Bytes test2 = struct_pack(&a, "<BB*I", 0x11, 0x22, &inner, 0xCCCCCCCC);
    print_bytes("  Expected: 11 22 AA AA BB BB CC CC CC CC", test2);
    printf("  Expected length: 10, got: %zu\n\n", test2.len);
    arena_reset(&a);

    // Test 3: bytes_pad_even - even length (no change)
    printf("Test 3: bytes_pad_even - even length\n");
    Bytes even = struct_pack(&a, "<HH", 0x1234, 0x5678);  // 4 bytes, already even
    Bytes padded_even = bytes_pad_even(&a, even);
    print_bytes("  Original (even):", even);
    print_bytes("  After pad_even:", padded_even);
    printf("  Should be same: %s\n\n", (even.data == padded_even.data) ? "YES" : "NO");
    arena_reset(&a);

    // Test 4: bytes_pad_even - odd length (adds padding)
    printf("Test 4: bytes_pad_even - odd length\n");
    Bytes odd = struct_pack(&a, "<BHH", 0x11, 0x2233, 0x4455);  // 5 bytes, odd
    print_bytes("  Original (odd):", odd);
    Bytes padded_odd = bytes_pad_even(&a, odd);
    print_bytes("  After pad_even:", padded_odd);
    printf("  Expected length: 6, got: %zu\n\n", padded_odd.len);
    arena_reset(&a);

    // Test 5: Combined - realistic use case
    printf("Test 5: Combined realistic use case\n");
    Bytes path = struct_pack(&a, "<BBBB", 0x20, 0xB2, 0x24, 0x01);
    Bytes realistic = struct_pack(&a, "<BB*H", 0x03, 0x02, &path, 0x0007);
    print_bytes("  Expected: 03 02 20 B2 24 01 07 00", realistic);
    printf("  Expected length: 8, got: %zu\n\n", realistic.len);

    arena_free(&a);
    printf("All tests completed!\n");
    return 0;
}
