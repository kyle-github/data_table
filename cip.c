#include "cip.h"
#include <stdio.h>
#include <string.h>

// ==========================================
// EIP ENCAPSULATION
// ==========================================

Bytes eip_encode_header(Arena *a, uint16_t command, uint32_t session_handle, Bytes payload) {
    // Header: Command(2) Length(2) Session(4) Status(4) Context(8) Options(4) = 24 bytes
    Bytes header = struct_pack(a, "<HHII8xI", command, (uint16_t)payload.len, session_handle,
                               0,   // status
                               0);  // options
    return bytes_concat(a, 2, header, payload);
}

// ==========================================
// CPF (COMMON PACKET FORMAT)
// ==========================================

Bytes cpf_encode_unconnected(Arena *a, Bytes payload) {
    // Interface(4) Timeout(2) ItemCount(2) NullAddr(type=0x0000,len=0x0000) DataItem(type=0x00B2,len)
    Bytes cpf = struct_pack(a, "<IHHHHHH", 0, 1, 2,          // interface, timeout, item_count
                            0x0000, 0x0000,                  // null address item (type, len)
                            0x00B2, (uint16_t)payload.len);  // unconnected data item (type, len)
    return bytes_concat(a, 2, cpf, payload);
}

Bytes cpf_encode_connected(Arena *a, uint32_t conn_id, uint16_t conn_sequence_num, Bytes payload) {
    // Prepend sequence number to payload
    Bytes cip_with_seq = struct_pack(a, "<H*", conn_sequence_num, &payload);

    Bytes cpf = struct_pack(a, "<IHH", 0, 0, 2);                                 // interface, timeout, item_count
    Bytes addr_item = struct_pack(a, "<HHI", 0x00A1, 4, conn_id);                // connected address
    Bytes data_hdr = struct_pack(a, "<HH", 0x00B1, (uint16_t)cip_with_seq.len);  // connected data

    return bytes_concat(a, 4, cpf, addr_item, data_hdr, cip_with_seq);
}

// ==========================================
// CIP SERVICE ENCODING
// ==========================================

Bytes cip_encode_object_service(Arena *a, uint8_t service, uint16_t class_id, uint16_t instance_id, Bytes service_data) {
    Bytes path = create_cip_class_path(a, (uint8_t)class_id, instance_id);
    return struct_pack(a, "<BB**", service, (uint8_t)(path.len / 2), &path, &service_data);
}

Bytes cip_encode_unconnected(Arena *a, Bytes payload, Bytes route) {
    // Service 0x52 to Connection Manager (class 0x06, instance 0x01)
    Bytes cm_path = struct_pack(a, "<BBBB", 0x20, 0x06, 0x24, 0x01);

    Bytes head = bytes_concat(a, 3, struct_pack(a, "<BB", 0x52, 2),  // service, path_len (2 words)
                              cm_path, struct_pack(a, "<BBH", 0x0A, 0x09, (uint16_t)payload.len));

    // Pad inner payload to even length before appending route
    payload = bytes_pad_even(a, payload);

    // Route: path_size_words(1) + reserved(1) + route_data
    Bytes route_header = struct_pack(a, "<BB", (uint8_t)(route.len / 2), 0x00);

    return bytes_concat(a, 4, head, payload, route_header, route);
}

// ==========================================
// FORWARD OPEN / CLOSE
// ==========================================

Bytes cip_encode_forward_open_payload(Arena *a, ForwardOpenParams params, Bytes device_mr_route) {
    // Priority/tick(1) + timeout_ticks(1) + O->T conn ID(4) + T->O conn ID(4) +
    // conn serial(2) + vendor(2) + originator serial(4)
    Bytes ids = struct_pack(a, "<BBIIHHI", 0x0A, 0x0E,  // priority/tick, timeout ticks
                            params.ot_connection_id, params.to_connection_id, params.connection_serial, params.vendor_id,
                            params.originator_serial);

    // Timeout multiplier(1) + reserved(3) + O->T RPI(4) + O->T params(2) +
    // T->O RPI(4) + T->O params(2) + transport(1)
    Bytes timing = struct_pack(a, "<B3xIHIHB", params.timeout_multiplier, params.ot_rpi, params.ot_params, params.to_rpi,
                               params.to_params, params.transport_trigger);

    // Connection path: path_size_words(1) + route_data
    Bytes path_hdr = struct_pack(a, "<B", (uint8_t)(device_mr_route.len / 2));

    return bytes_concat(a, 4, ids, timing, path_hdr, device_mr_route);
}

Bytes cip_encode_forward_close_payload(Arena *a, uint16_t connection_serial, uint16_t vendor_id, uint32_t originator_serial,
                                       Bytes device_mr_route) {
    // Priority/tick(1) + timeout_ticks(1) + conn serial(2) + vendor(2) + originator serial(4) + reserved(1)
    Bytes params = struct_pack(a, "<BBHHIBB", 0x0A, 0x09, connection_serial, vendor_id, originator_serial,
                               0,                                    // reserved
                               (uint8_t)(device_mr_route.len / 2));  // path size

    return bytes_concat(a, 2, params, device_mr_route);
}

// ==========================================
// CIP PATH / SEGMENT BUILDERS
// ==========================================

Bytes create_cip_class_path(Arena *a, uint8_t class_id, uint32_t instance_id) {
    if(instance_id <= 0xFF) {
        return struct_pack(a, "<BBBB", 0x20, class_id, 0x24, (uint8_t)instance_id);
    } else {
        return struct_pack(a, "<BBBBH", 0x20, class_id, 0x25, 0x00, (uint16_t)instance_id);
    }
}

Bytes cip_encode_port_segment(Arena *a, uint8_t port, uint8_t slot) { return struct_pack(a, "<BB", port, slot); }

Bytes cip_encode_mr_route(Arena *a, uint8_t port, uint8_t slot) {
    // Port/slot segment + class 0x02 (Message Router), instance 0x01
    return struct_pack(a, "<BBBBBB", port, slot, 0x20, 0x02, 0x24, 0x01);
}

Bytes encode_tag_name(Arena *a, const char *tag) {
    size_t len = strlen(tag);
    if(len % 2 != 0) {
        return struct_pack(a, "<BBsB", 0x91, (uint8_t)len, tag, 0x00);
    } else {
        return struct_pack(a, "<BBs", 0x91, (uint8_t)len, tag);
    }
}

// ==========================================
// CIP RESPONSE PARSING
// ==========================================

CipResponse eip_parse_response(Bytes response) {
    CipResponse result = {{0, 0, 0xFF, 0}, {NULL, 0}};

    if(response.len < 40) { return result; }

    // EIP Header (24 bytes)
    uint16_t eip_command = 0, eip_length = 0;
    uint32_t eip_session = 0, eip_status = 0, eip_options = 0;
    uint64_t eip_context = 0;

    Bytes remaining =
        struct_unpack(response, "<HHIIQI", &eip_command, &eip_length, &eip_session, &eip_status, &eip_context, &eip_options);

    // CPF Header (8 bytes)
    uint32_t cpf_interface = 0;
    uint16_t cpf_timeout = 0, cpf_item_count = 0;

    remaining = struct_unpack(remaining, "<IHH", &cpf_interface, &cpf_timeout, &cpf_item_count);

    // Address Item: type(2) + length(2) + data(length bytes)
    uint16_t addr_type = 0, addr_len = 0;
    remaining = struct_unpack(remaining, "<HH", &addr_type, &addr_len);

    if(addr_len > 0) {
        if(remaining.len < addr_len) {
            result.header.status = 0xFF;
            return result;
        }
        remaining = bytes_slice(remaining, addr_len, remaining.len - addr_len);
    }

    // Data Item: type(2) + length(2)
    uint16_t data_type = 0, data_len = 0;
    remaining = struct_unpack(remaining, "<HH", &data_type, &data_len);

    // For connected data items (0x00B1), skip the 2-byte sequence number
    if(data_type == 0x00B1 && remaining.len >= 2) { remaining = bytes_slice(remaining, 2, remaining.len - 2); }

    if(remaining.len < 4) {
        result.header.status = 0xFF;
        return result;
    }

    remaining = struct_unpack(remaining, "<BBBB", &result.header.srv, &result.header.reserved, &result.header.status,
                              &result.header.ext_status_words);

    if(remaining.data == NULL) {
        result.header.status = 0xFF;
        return result;
    }

    result.payload = remaining;
    return result;
}

Bytes cip_get_response_data(CipResponse cip_resp) {
    size_t ext_status_bytes = (size_t)cip_resp.header.ext_status_words * 2;
    if(ext_status_bytes >= cip_resp.payload.len) { return (Bytes){NULL, 0}; }
    return bytes_slice(cip_resp.payload, ext_status_bytes, cip_resp.payload.len - ext_status_bytes);
}

ForwardOpenResponse cip_parse_forward_open_response(CipResponse cip_resp) {
    ForwardOpenResponse fo = {0};
    fo.valid = false;

    if(cip_resp.header.status != 0) { return fo; }

    Bytes payload = cip_get_response_data(cip_resp);
    // Forward Open response: O->T conn ID(4) + T->O conn ID(4) + conn serial(2) + vendor(2) + originator serial(4) + ...
    if(payload.data == NULL || payload.len < 16) { return fo; }

    struct_unpack(payload, "<IIHHI", &fo.ot_connection_id, &fo.to_connection_id, &fo.connection_serial, &fo.vendor_id,
                  &fo.originator_serial);

    fo.valid = true;
    return fo;
}

// ==========================================
// TREND OBJECT PAYLOAD BUILDERS
// ==========================================

Bytes create_trend_payload(Arena *a, uint32_t buffer_size, uint8_t num_tags) {
    return bytes_concat(a, 3, struct_pack(a, "<HHI", 2, 8, buffer_size), struct_pack(a, "<H", 3), pack_uint8(a, num_tags));
}

Bytes create_set_attrs_payload(Arena *a, uint32_t sample_rate_us, uint8_t state) {
    return bytes_concat(a, 3, struct_pack(a, "<HHI", 2, 1, sample_rate_us), struct_pack(a, "<H", 5), pack_uint8(a, state));
}

Bytes create_add_tag_payload(Arena *a, const char *tag_name) {
    Bytes sym_path = encode_tag_name(a, tag_name);
    uint8_t path_size_words = (uint8_t)(sym_path.len / 2);

    return bytes_concat(a, 4, struct_pack(a, "<H", 1), struct_pack(a, "<BBB", 1, 1, path_size_words), sym_path,
                        struct_pack(a, "<I", 0xFFFFFFFF));
}

Bytes create_remove_tag_payload(Arena *a, uint16_t tag_index) { return struct_pack(a, "<H", tag_index); }

// ==========================================
// CIP RESPONSE PARSERS
// ==========================================

void parse_and_print_samples(Bytes payload, uint16_t data_type) {
    Bytes current = payload;
    while(current.len >= 10) {
        uint16_t count = 0;
        uint32_t timestamp = 0;
        uint32_t value_raw = 0;
        current = struct_unpack(current, "<HII", &count, &timestamp, &value_raw);

        if(current.data == NULL) { break; }

        if(data_type == 0xCA) {
            float value_float = *(float *)&value_raw;
            printf("  Sample %u: value=%.4f timestamp=%u us\n", count, value_float, timestamp);
        } else {
            int32_t value_signed = (int32_t)value_raw;
            printf("  Sample %u: value=%d (0x%08X) timestamp=%u us\n", count, value_signed, value_raw, timestamp);
        }
    }
}
