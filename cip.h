#ifndef CIP_H
#define CIP_H

#include "arena.h"
#include "bytes.h"
#include <stdint.h>
#include <stddef.h>

// ==========================================
// GENERIC CIP/EIP PACKET BUILDERS
// ==========================================

// Generic CIP Request Builder
// Builds: [Service][PathLen][Path][Data]
Bytes create_cip_request(Arena *a, uint8_t service_code, Bytes path, Bytes data);

// Wraps a CIP request in an Unconnected Send (Service 0x52) to route it
// Routes to: Backplane Port 1, Slot 4
Bytes create_unconnected_send(Arena *a, Bytes inner_request);

// Wraps CIP data in EtherNet/IP SendRRData (0x6F) packet
Bytes create_eip_packet(Arena *a, uint32_t session_handle, Bytes cip_data);

// ==========================================
// CIP RESPONSE PARSING
// ==========================================

// CIP Response Header (4 bytes)
typedef struct {
    uint8_t srv;                  // Service code
    uint8_t reserved;             // Reserved
    uint8_t status;               // Status code
    uint8_t ext_status_words;     // Extended status size in uint16_t words
} CipResponseHeader;

// CIP response with header and payload
// If ext_status_words > 0, extended status is in the first ext_status_words*2 bytes of payload
// Remaining bytes (if any) are the actual response data
typedef struct {
    CipResponseHeader header;
    Bytes payload;  // Includes extended status (if present) + response data
} CipResponse;

// Parse CIP response from unconnected send wrapper
// Finds the CIP response within the EIP packet and parses it
// Returns response header and payload (includes extended status bytes if present)
CipResponse parse_cip_response(Bytes response);

// Extract data portion from CIP response payload
// Skips extended status words if present
// Returns: Bytes containing only the response data (after extended status)
Bytes cip_get_response_data(CipResponse cip_resp);

// ==========================================
// CIP PATH BUILDERS
// ==========================================

// Helper for Class/Instance paths (e.g., 20 B2 24 01)
// Handles both 8-bit and 16-bit instance IDs with proper padding
// Example: create_cip_class_path(a, 0xB2, 1) -> "20 B2 24 01"
Bytes create_cip_class_path(Arena *a, uint8_t class_id, uint32_t instance_id);

// Encodes a string tag into CIP Path format (ANSI Extended Symbol Segment)
// Format: [0x91][length][string_bytes][optional_padding_if_odd]
// Example: encode_tag_name(a, "Test") -> 0x91 0x04 'T' 'e' 's' 't' 0x00
Bytes encode_tag_name(Arena *a, const char *tag);

// ==========================================
// TREND OBJECT PAYLOAD BUILDERS
// ==========================================

// Create Trend payload for service 0x08
// Format: [attr_count:u16=2][attr_id:u16=8][buffer_size:u32][attr_id:u16=3][num_tags:u8]
Bytes create_trend_payload(Arena *a, uint32_t buffer_size, uint8_t num_tags);

// SetAttributeList payload for service 0x04
// Format: [attr_count:u16=2][attr_id:u16=1][sample_rate:u32][attr_id:u16=5][state:u8]
Bytes create_set_attrs_payload(Arena *a, uint32_t sample_rate_us, uint8_t state);

// AddTag payload for service 0x4E
// Format: [num_tags:u16=1][tag_index:u8=1][type:u8=1][path_size_words:u8][symbolic_path][mask:u32=0xFFFFFFFF]
Bytes create_add_tag_payload(Arena *a, const char *tag_name);

// RemoveTag payload for service 0x4F
// Format: [tag_index:u16]
Bytes create_remove_tag_payload(Arena *a, uint16_t tag_index);

// ==========================================
// CIP RESPONSE PARSERS
// ==========================================

// Parse samples from ReadData response (service 0x4C)
// Format: [count:u16][timestamp:u32][value:u32] repeated
// data_type: 0xC4=DINT, 0xCA=REAL, etc.
void parse_and_print_samples(Bytes payload, uint16_t data_type);

#endif
