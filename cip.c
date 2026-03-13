#include "cip.h"
#include <stdio.h>
#include <string.h>

// ==========================================
// GENERIC CIP/EIP PACKET BUILDERS
// ==========================================

// Generic CIP Request Builder
Bytes create_cip_request(Arena *a, uint8_t service_code, Bytes path, Bytes data) {
    // Request: [Service] [PathLen] [Path] [Data]
    return struct_pack(a, "<BB**", service_code, (uint8_t)(path.len / 2), &path, &data);
}

// Wraps a CIP request in an Unconnected Send (Service 0x52) to route it
// e.g. Route "1,4" -> Backplane (1), Slot 4
Bytes create_unconnected_send(Arena *a, Bytes inner_request) {
    // Path to Connection Manager: Class 0x06, Instance 0x01
    Bytes cm_path = struct_pack(a, "<BBBB", 0x20, 0x06, 0x24, 0x01);

    // Unconnected Send header: service, path_len, cm_path, priority, timeout, msg_len
    Bytes head = bytes_concat(a, 3,
                               struct_pack(a, "<BB", 0x52, 2),  // Service, Path len (2 words)
                               cm_path,
                               struct_pack(a, "<BBH", 0x0A, 0x09, (uint16_t)inner_request.len));  // Priority, Timeout, msg_len

    // Pad inner request to even length
    inner_request = bytes_pad_even(a, inner_request);

    // Route: path_len (1 word) + port 1 + link 4
    Bytes tail = struct_pack(a, "<BBB", 1, 0x01, 0x04);

    return bytes_concat(a, 3, head, inner_request, tail);
}

// Wraps in EtherNet/IP Header (SendRRData 0x6F)
Bytes create_eip_packet(Arena *a, uint32_t session_handle, Bytes cip_data) {
    // Payload header: Interface(4) + Timeout(2) + ItemCount(2) + AddressItem(type+len) + DataItem(type+len)
    Bytes payload_header = struct_pack(a, "<IHHHHH",
                                       0, 0, 2,                    // interface, timeout, item_count
                                       0x0000, 0x0000,             // address item (type, len)
                                       0x00B2, (uint16_t)cip_data.len);  // data item (type, len)

    size_t total_data_len = payload_header.len + cip_data.len;

    // EIP Header: Command(2), Len(2), Session(4), Status(4), Context(8), Options(4)
    Bytes header = struct_pack(a, "<HHII8xI", 0x006F, (uint16_t)total_data_len, session_handle, 0, 0);

    return bytes_concat(a, 3, header, payload_header, cip_data);
}

// ==========================================
// CIP RESPONSE PARSING
// ==========================================

// Parse CIP response from unconnected send wrapper
// Response structure:
//   EIP header (24) + address item (4) + data item (4) + reserved (2) + CIP response
//   CIP response: [srv][reserved][status][ext_status_words][ext_status... if words>0][data...]
CipResponse parse_cip_response(Bytes response) {
    CipResponse result = {{0, 0, 0xFF, 0}, {NULL, 0}};

    // Locate CIP response within EIP packet
    // Fixed offset: EIP header (24) + address item (4) + data item (4) = 32
    size_t cip_offset = 32;

    // Skip EIP reserved bytes (2 bytes after CPF items)
    cip_offset += 2;

    if(response.len < cip_offset + 4) {
        result.header.status = 0xFF;  // Invalid - not enough data for header
        return result;
    }

    // Parse CIP response header using struct_unpack
    Bytes cip_data = bytes_slice(response, cip_offset, response.len - cip_offset);
    Bytes remaining = struct_unpack(cip_data, "<BBBB",
                                    &result.header.srv,
                                    &result.header.reserved,
                                    &result.header.status,
                                    &result.header.ext_status_words);

    if(remaining.data == NULL) {
        result.header.status = 0xFF;  // Parse error
        return result;
    }

    // Extract payload (extended status + data)
    result.payload = remaining;

    return result;
}

// Extract data portion from CIP response payload
// Skips extended status words if present
Bytes cip_get_response_data(CipResponse cip_resp) {
    // Skip extended status bytes if present
    size_t ext_status_bytes = (size_t)cip_resp.header.ext_status_words * 2;

    if(ext_status_bytes >= cip_resp.payload.len) {
        // No data after extended status
        return (Bytes){NULL, 0};
    }

    return bytes_slice(cip_resp.payload, ext_status_bytes, cip_resp.payload.len - ext_status_bytes);
}

// ==========================================
// CIP PATH BUILDERS
// ==========================================

// Helper for Class/Instance paths (e.g., 20 B2 24 01)
Bytes create_cip_class_path(Arena *a, uint8_t class_id, uint32_t instance_id) {
    if(instance_id <= 0xFF) {
        return struct_pack(a, "<BBBB", 0x20, class_id, 0x24, (uint8_t)instance_id);
    } else {
        // For instance > 0xFF: segment_type, padding, value
        return struct_pack(a, "<BBBBH", 0x20, class_id, 0x25, 0x00, (uint16_t)instance_id);
    }
}

// Encodes a string tag into CIP Path format (ANSI Extended Symbol Segment)
// e.g. "Test" -> 0x91 0x04 'T' 'e' 's' 't' 0x00 (padded to even length)
Bytes encode_tag_name(Arena *a, const char *tag) {
    size_t len = strlen(tag);

    // ANSI Extended Symbol Segment: 0x91 [length] [string] [padding if odd]
    if(len % 2 != 0) {
        // Odd length: add padding byte
        return struct_pack(a, "<BBsB", 0x91, (uint8_t)len, tag, 0x00);
    } else {
        // Even length: no padding needed
        return struct_pack(a, "<BBs", 0x91, (uint8_t)len, tag);
    }
}

// ==========================================
// TREND OBJECT PAYLOAD BUILDERS
// ==========================================

// Create Trend payload: [attr_count:u16=2][attr_id:u16=8][buffer_size:u32][attr_id:u16=3][num_tags:u8]
Bytes create_trend_payload(Arena *a, uint32_t buffer_size, uint8_t num_tags) {
    return bytes_concat(a, 5,
                        struct_pack(a, "<HHI", 2, 8, buffer_size),
                        struct_pack(a, "<H", 3),
                        pack_uint8(a, num_tags));
}

// SetAttributeList payload: [attr_count:u16=2][attr_id:u16=1][sample_rate:u32][attr_id:u16=5][state:u8]
Bytes create_set_attrs_payload(Arena *a, uint32_t sample_rate_us, uint8_t state) {
    return bytes_concat(a, 4,
                        struct_pack(a, "<HHI", 2, 1, sample_rate_us),
                        struct_pack(a, "<H", 5),
                        pack_uint8(a, state));
}

// AddTag payload: [num_tags:u16=1][tag_index:u8=1][type:u8=1][path_size_words:u8][symbolic_path][mask:u32=0xFFFFFFFF]
Bytes create_add_tag_payload(Arena *a, const char *tag_name) {
    Bytes sym_path = encode_tag_name(a, tag_name);
    uint8_t path_size_words = (uint8_t)(sym_path.len / 2);

    return bytes_concat(a, 5,
                        struct_pack(a, "<H", 1),  // num_tags
                        struct_pack(a, "<BBB", 1, 1, path_size_words),  // tag_index, type, path_size
                        sym_path,
                        struct_pack(a, "<I", 0xFFFFFFFF));  // mask
}

// RemoveTag payload: [tag_index:u16=1]
Bytes create_remove_tag_payload(Arena *a, uint16_t tag_index) {
    return struct_pack(a, "<H", tag_index);
}

// ==========================================
// CIP RESPONSE PARSERS
// ==========================================

// Parse samples from ReadData response
// Format: [count:u16][timestamp:u32][value:u32] repeated
// data_type: 0xC4=DINT, 0xCA=REAL, etc.
void parse_and_print_samples(Bytes payload, uint16_t data_type) {
    Bytes current = payload;
    while(current.len >= 10) {
        uint16_t count = 0;
        uint32_t timestamp = 0;
        uint32_t value_raw = 0;
        current = struct_unpack(current, "<HII", &count, &timestamp, &value_raw);

        if(current.data == NULL) break;  // error

        if(data_type == 0xCA) {  // REAL (float)
            float value_float = *(float *)&value_raw;
            printf("  Sample %u: value=%.4f timestamp=%u us\n", count, value_float, timestamp);
        } else {  // DINT and others as signed int
            int32_t value_signed = (int32_t)value_raw;
            printf("  Sample %u: value=%d (0x%08X) timestamp=%u us\n", count, value_signed, value_raw, timestamp);
        }
    }
}
