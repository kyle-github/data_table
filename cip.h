#ifndef CIP_H
#define CIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "bytes.h"

// ==========================================
// EIP ENCAPSULATION
// ==========================================

#define EIP_CMD_REGISTER_SESSION 0x0065
#define EIP_CMD_SEND_RR_DATA 0x006F
#define EIP_CMD_SEND_UNIT_DATA 0x0070

// Encode EIP encapsulation header + payload
// Header: Command(2) Length(2) Session(4) Status(4) Context(8) Options(4)
Bytes eip_encode_header(Arena *a, uint16_t command, uint32_t session_handle, Bytes payload);

// ==========================================
// CPF (COMMON PACKET FORMAT)
// ==========================================

// Encode CPF for unconnected messaging (SendRRData)
// Format: Interface(4) Timeout(2) ItemCount(2) NullAddr(4) DataItem(type+len) + payload
Bytes cpf_encode_unconnected(Arena *a, Bytes payload);

// Encode CPF for connected messaging (SendUnitData)
// Format: Interface(4) Timeout(2) ItemCount(2) ConnAddr(8) DataItem(type+len+seq) + payload
Bytes cpf_encode_connected(Arena *a, uint32_t conn_id, uint16_t conn_sequence_num, Bytes payload);

// ==========================================
// CIP SERVICE ENCODING
// ==========================================

// Encode a CIP service request with a class/instance path
// Format: [Service][PathLen_words][class_segment][instance_segment][service_data]
Bytes cip_encode_object_service(Arena *a, uint8_t service, uint16_t class_id, uint16_t instance_id, Bytes service_data);

// Wrap a CIP request in an Unconnected Send (service 0x52 to Connection Manager)
// payload = the inner CIP request to send through the route
// route = port/link path to the target device (e.g., backplane port 1, slot 4)
Bytes cip_encode_unconnected(Arena *a, Bytes payload, Bytes route);

// ==========================================
// FORWARD OPEN / CLOSE
// ==========================================

typedef struct {
    uint32_t ot_connection_id;
    uint32_t to_connection_id;
    uint16_t connection_serial;
    uint16_t vendor_id;
    uint32_t originator_serial;
    uint8_t timeout_multiplier;
    uint32_t ot_rpi;     // O->T RPI in microseconds
    uint16_t ot_params;  // O->T network connection parameters
    uint32_t to_rpi;     // T->O RPI in microseconds
    uint16_t to_params;  // T->O network connection parameters
    uint8_t transport_trigger;
} ForwardOpenParams;

// Encode Forward Open payload (tick/timeout + parameters + connection path)
// device_mr_route = route to the end device's Message Router
//                   (e.g., port 1 slot 4 + class 0x02 instance 0x01)
Bytes cip_encode_forward_open_payload(Arena *a, ForwardOpenParams params, Bytes device_mr_route);

// Encode Forward Close payload
// device_mr_route = same route used in Forward Open
Bytes cip_encode_forward_close_payload(Arena *a, uint16_t connection_serial, uint16_t vendor_id, uint32_t originator_serial,
                                       Bytes device_mr_route);

typedef struct {
    uint32_t ot_connection_id;
    uint32_t to_connection_id;
    uint16_t connection_serial;
    uint16_t vendor_id;
    uint32_t originator_serial;
    bool valid;
} ForwardOpenResponse;

// ==========================================
// CIP PATH / SEGMENT BUILDERS
// ==========================================

// Build a class/instance path: [0x20][class][0x24/0x25][instance]
Bytes create_cip_class_path(Arena *a, uint8_t class_id, uint32_t instance_id);

// Encode a port/link segment (e.g., backplane port 1, slot N)
Bytes cip_encode_port_segment(Arena *a, uint8_t port, uint8_t slot);

// Encode a route to a device's Message Router: port/slot + class 2, instance 1
Bytes cip_encode_mr_route(Arena *a, uint8_t port, uint8_t slot);

// Encode tag name as ANSI Extended Symbol Segment
Bytes encode_tag_name(Arena *a, const char *tag);

// ==========================================
// CIP RESPONSE PARSING
// ==========================================

typedef struct {
    uint8_t srv;
    uint8_t reserved;
    uint8_t status;
    uint8_t ext_status_words;
} CipResponseHeader;

typedef struct {
    CipResponseHeader header;
    Bytes payload;  // extended status (if any) + response data
} CipResponse;

// Parse EIP response — extracts CIP response from within EIP/CPF envelope
// Works for both unconnected (SendRRData) and connected (SendUnitData)
CipResponse eip_parse_response(Bytes response);

// Extract data portion from CIP response (skips extended status words)
Bytes cip_get_response_data(CipResponse cip_resp);

// Parse Forward Open response payload
ForwardOpenResponse cip_parse_forward_open_response(CipResponse cip_resp);

// ==========================================
// TREND OBJECT PAYLOAD BUILDERS
// ==========================================

Bytes create_trend_payload(Arena *a, uint32_t buffer_size, uint8_t num_tags);
Bytes create_set_attrs_payload(Arena *a, uint32_t sample_rate_us, uint8_t state);
Bytes create_add_tag_payload(Arena *a, const char *tag_name);
Bytes create_remove_tag_payload(Arena *a, uint16_t tag_index);
void parse_and_print_samples(Bytes payload, uint16_t data_type);

#endif
