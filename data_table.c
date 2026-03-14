#include "arena.h"
#include "bytes.h"
#include "cip.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ==========================================
// SIGNAL HANDLING
// ==========================================

// Global signal flag
volatile sig_atomic_t keep_running = 1;

void sig_handler(int _) {
    (void)_;
    keep_running = 0;
}

// ==========================================
// 1. EIP CONNECTION MANAGEMENT
// ==========================================

// EIP Connection structure (forward declaration for functions that use it)
typedef struct {
    int sock_fd;
    uint32_t session_handle;
    Arena *arena;
    // Connected transport parameters
    bool connected;
    uint32_t cpid;        // O->T connection ID
    uint16_t sssn;        // session serial number
    uint16_t sequence;    // sequence number for connected messages
} EipConnection;

// Forward declaration of send_cip_command (defined later)
Bytes send_cip_command(EipConnection *conn, Bytes cip_req);

// Read a tag value using CIP Read Tag (service 0x4C on symbolic path)
// Returns the tag data type (0xC4=DINT, 0xCA=REAL, etc.)
uint16_t read_tag_value(EipConnection *conn, const char *tag_name) {
    printf("[*] Reading tag '%s'...\n", tag_name);

    Bytes sym_path = encode_tag_name(conn->arena, tag_name);
    uint8_t path_words = (uint8_t)(sym_path.len / 2);

    Bytes cip_req = struct_pack(conn->arena, "<BB*H", 0x4C, path_words, &sym_path, (uint16_t)1);

    Bytes response = send_cip_command(conn, cip_req);

    uint16_t data_type = 0;
    if(response.len > 0) {
        CipResponse cip_resp = parse_cip_response(response);
        if(cip_resp.header.status == 0) {
            Bytes payload = cip_get_response_data(cip_resp);
            if(payload.data != NULL && payload.len >= 6) {
                uint32_t value_raw = 0;
                struct_unpack(payload, "<HI", &data_type, &value_raw);  // ignore remaining

                const char *type_name = "UNKNOWN";
                if(data_type == 0xC1) type_name = "BOOL";
                else if(data_type == 0xC2) type_name = "SINT";
                else if(data_type == 0xC3) type_name = "INT";
                else if(data_type == 0xC4) type_name = "DINT";
                else if(data_type == 0xCA) type_name = "REAL";

                if(data_type == 0xCA) {  // REAL
                    float val = *(float *)&value_raw;
                    printf("  %s = %.4f\n", type_name, val);
                } else {  // Signed integer types
                    int32_t val = (int32_t)value_raw;
                    printf("  %s = %d\n", type_name, val);
                }
            }
        } else {
            printf("  Read failed: status=0x%02X\n", cip_resp.header.status);
        }
    }
    arena_reset(conn->arena);
    return data_type;
}

// Get Trend Attributes (service 0x03)
void get_trend_attributes(EipConnection *conn, uint32_t instance_id) {
    printf("[*] Getting Trend Attributes for instance %u...\n", instance_id);

    Bytes path = create_cip_class_path(conn->arena, 0xB2, instance_id);

    // Pack CIP request: service, path_len, path, attr_count, attr_ids
    Bytes cip_req = struct_pack(conn->arena, "<BB*HHHHHHHH",
                                0x03, (uint8_t)(path.len / 2),
                                &path,
                                7, 1, 3, 5, 6, 7, 8, 0x0A);

    Bytes response = send_cip_command(conn, cip_req);

    if(response.len > 0) {
        CipResponse cip_resp = parse_cip_response(response);
        printf("  Status: 0x%02X\n", cip_resp.header.status);
        if(cip_resp.header.status == 0) {
            Bytes payload = cip_get_response_data(cip_resp);
            if(payload.data != NULL && payload.len >= 2) {
                uint16_t attr_count_resp = 0;
                struct_unpack(payload, "<H", &attr_count_resp);
                printf("  Attributes: %u\n", attr_count_resp);
            }
        }
    }
    arena_reset(conn->arena);
}

// ==========================================
// 2. EIP CONNECTION FUNCTIONS
// ==========================================


// Connects to the target and returns a connection object
EipConnection eip_connect(Arena *a, const char *ip, int port) {
    EipConnection conn = {0};
    conn.arena = a;
    conn.sock_fd = -1;
    conn.session_handle = 0;
    conn.connected = false;
    conn.cpid = 0;
    conn.sssn = 0;
    conn.sequence = 0;
    conn.sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(conn.sock_fd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    fprintf(stderr, "[DEBUG] Attempting to connect to %s:%d\n", ip, port);
    fprintf(stderr, "[DEBUG] Socket fd: %d\n", conn.sock_fd);

    if(inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address/ Address not supported \n");
        close(conn.sock_fd);
        exit(1);
    }

    fprintf(stderr, "[DEBUG] Address parsed successfully, calling connect()...\n");

    if(connect(conn.sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        fprintf(stderr, "[DEBUG] connect() failed with errno: %d\n", errno);
        perror("Connection Failed");
        close(conn.sock_fd);
        exit(1);
    }

    fprintf(stderr, "[DEBUG] Connection successful!\n");
    fflush(stderr);

    return conn;
}

// Disconnects from the target
void eip_disconnect(EipConnection *conn) {
    if(conn && conn->sock_fd >= 0) {
        // Optional: Send UnRegisterSession
        close(conn->sock_fd);
        conn->sock_fd = -1;
    }
}

// Sends a RegisterSession command and stores the session handle
bool eip_register_session(EipConnection *conn) {
    fprintf(stderr, "[DEBUG] eip_register_session starting\n");
    fflush(stderr);

    // EIP Header for RegisterSession (0x65) + Payload
    // Header: Command(2), Len(2), Session(4), Status(4), Context(8), Options(4)
    // Payload: Protocol(2), Flags(2)
    Bytes packet = struct_pack(conn->arena, "<HHII8xIHH", 0x0065, 4, 0, 0, 0, 1, 0);

    if(send(conn->sock_fd, packet.data, packet.len, 0) < 0) {
        perror("RegisterSession send failed");
        return false;
    }

    // Reset arena after sending request, allocate fresh space for response
    arena_reset(conn->arena);
    uint8_t *buf = arena_alloc(conn->arena, 256);  // Response is small
    ssize_t received = recv(conn->sock_fd, buf, 256, 0);
    if(received < 24) {  // Must be at least a full EIP header
        fprintf(stderr, "RegisterSession recv failed or got truncated response\n");
        return false;
    }

    // EIP Status check (bytes 8-11)
    uint32_t eip_status = 0;
    struct_unpack(bytes_slice(bytes_from_buf(buf, received), 8, received - 8), "<I", &eip_status);
    if(eip_status != 0) {
        fprintf(stderr, "RegisterSession failed with EIP status: 0x%08X\n", eip_status);
        return false;
    }

    // Extract session handle (bytes 4-7)
    struct_unpack(bytes_slice(bytes_from_buf(buf, received), 4, received - 4), "<I", &conn->session_handle);
    printf("[*] Session registered with handle: 0x%08X\n", conn->session_handle);

    return true;
}

// Sends data using SendRRData and returns the response
Bytes eip_send_rr_data(Arena *a, EipConnection *conn, Bytes cip_data) {
    Bytes packet = create_eip_packet(conn->arena, conn->session_handle, cip_data);

    bytes_hexdump(packet, "[SEND]");

    if(send(conn->sock_fd, packet.data, packet.len, 0) < 0) {
        perror("SendRRData send failed");
        return (Bytes){NULL, 0};
    }

    // Reset arena after sending request, allocate fresh space for response
    arena_reset(a);
    uint8_t *buf = arena_alloc(a, 4096);

    ssize_t received = recv(conn->sock_fd, buf, 4096, 0);
    if(received < 0) {
        perror("SendRRData recv failed");
        return (Bytes){NULL, 0};
    }

    Bytes response = (Bytes){buf, (size_t)received};
    bytes_hexdump(response, "[RECV]");

    return response;
}

// Sends CIP message using SendUnitData (0x70) on connected transport
// Returns the CIP response data (without EIP/CPF headers)
Bytes eip_send_unit_data(EipConnection *conn, Bytes cip_message) {
    fprintf(stderr, "[DEBUG] eip_send_unit_data: cpid=0x%08X, sssn=%u, seq=%u, connected=%d\n",
            conn->cpid, conn->sssn, conn->sequence, conn->connected);
    fflush(stderr);

    // Increment sequence number
    conn->sequence++;

    // Build CIP message with sequence number prefix
    Bytes cip_with_seq = struct_pack(conn->arena, "<H*", conn->sequence, &cip_message);

    // Build all CPF components
    Bytes cpf_header = struct_pack(conn->arena, "<IHH", 0, 0, 2);  // interface, timeout, item_count
    Bytes addr_item = struct_pack(conn->arena, "<HHI", 0xA1, 4, conn->cpid);  // connected address item
    Bytes data_item_header = struct_pack(conn->arena, "<HH", 0xB1, (uint16_t)cip_with_seq.len);

    // Concatenate: CPF header + Address Item + Data Item header + Data Item payload
    Bytes payload = bytes_concat(conn->arena, 4, cpf_header, addr_item, data_item_header, cip_with_seq);

    // Build EIP header with SendUnitData command (0x0070)
    // Format: Command(2), Length(2), Session(4), Status(4), Context(8), Options(4)
    fprintf(stderr, "[DEBUG] Header values: cmd=0x%04X, len=%zu, session=0x%08X, status=0, options=0\n",
            0x0070, payload.len, conn->session_handle);
    fflush(stderr);

    Bytes header = struct_pack(conn->arena, "<HHII8xI",
                              0x0070,                  // Command: SendUnitData
                              (uint16_t)payload.len,   // Length
                              conn->session_handle,    // Session
                              0,                       // Status
                              0);                      // Options (Context is padding)

    Bytes packet = bytes_concat(conn->arena, 2, header, payload);

    fprintf(stderr, "[DEBUG] eip_send_unit_data packet size: %zu bytes, hex:\n", packet.len);
    for(size_t i = 0; i < packet.len; i += 16) {
        fprintf(stderr, "  ");
        for(size_t j = i; j < (i + 16 < packet.len ? i + 16 : packet.len); j++) {
            fprintf(stderr, "%02X ", packet.data[j]);
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);

    if(send(conn->sock_fd, packet.data, packet.len, 0) < 0) {
        perror("SendUnitData send failed");
        return (Bytes){NULL, 0};
    }

    // Reset arena after sending, allocate fresh space for response
    arena_reset(conn->arena);
    uint8_t *buf = arena_alloc(conn->arena, 4096);

    ssize_t received = recv(conn->sock_fd, buf, 4096, 0);
    if(received < 0) {
        perror("SendUnitData recv failed");
        return (Bytes){NULL, 0};
    }

    Bytes response = (Bytes){buf, (size_t)received};
    bytes_hexdump(response, "[RECV]");

    // Parse response and extract CIP data from connected format
    // Response structure:
    // EIP header (24) + CPF header (8) + Address Item (8) + Data Item header (4) + CIP response
    if(response.len < 44) {  // Minimum for connected response
        fprintf(stderr, "SendUnitData response too short: %zu bytes\n", response.len);
        return (Bytes){NULL, 0};
    }

    // Skip EIP header (24 bytes) + CPF header (8 bytes)
    Bytes remaining = bytes_slice(response, 32, response.len - 32);

    // Skip Address Item (type 0xA1, len 4, data 4 bytes = 8 bytes total)
    if(remaining.len < 8) {
        fprintf(stderr, "SendUnitData response missing address item\n");
        return (Bytes){NULL, 0};
    }
    uint16_t addr_type = 0, addr_len = 0;
    remaining = struct_unpack(remaining, "<HH", &addr_type, &addr_len);
    if(remaining.data == NULL || remaining.len < addr_len) {
        fprintf(stderr, "SendUnitData response address parse error\n");
        return (Bytes){NULL, 0};
    }
    remaining = bytes_slice(remaining, addr_len, remaining.len - addr_len);

    // Parse Data Item header (type 0xB1, length)
    if(remaining.len < 4) {
        fprintf(stderr, "SendUnitData response missing data item header\n");
        return (Bytes){NULL, 0};
    }
    uint16_t data_type = 0, data_len = 0;
    remaining = struct_unpack(remaining, "<HH", &data_type, &data_len);
    if(remaining.data == NULL || remaining.len < data_len) {
        fprintf(stderr, "SendUnitData response data too short\n");
        return (Bytes){NULL, 0};
    }

    // Extract CIP response (skip sequence number at start)
    if(data_len < 2) {
        fprintf(stderr, "SendUnitData CIP response too short\n");
        return (Bytes){NULL, 0};
    }

    // Skip the 2-byte sequence number, return the rest as CIP response
    return bytes_slice(remaining, 2, data_len - 2);
}

// Forward Open (0x54) to establish connected transport
bool eip_forward_open(EipConnection *conn, uint8_t slot) {
    // Generate random connection IDs and serial number
    uint32_t ot_id = 0x80000000 | (rand() & 0xFFFF);
    uint32_t to_id = 0x803F0000 | (rand() & 0xFFFF);
    uint16_t conn_serial = (uint16_t)(rand() & 0xFFFF);

    // Connection path: backplane port 1 → slot → class 0x02, instance 0x01
    Bytes conn_path = struct_pack(conn->arena, "<BBBBBB", 0x01, slot, 0x20, 0x02, 0x24, 0x01);

    // Build complete Forward Open request matching Python implementation
    Bytes fo_data = bytes_concat(conn->arena, 4,
                                 // Service + path to Connection Manager
                                 struct_pack(conn->arena, "<BBBBBB", 0x54, 0x02, 0x20, 0x06, 0x24, 0x01),
                                 // Priority, timeout, connection IDs, serial, vendor, originator
                                 struct_pack(conn->arena, "<BBIIHHI",
                                             0x0A, 0x0E,      // priority/tick, timeout ticks
                                             ot_id,           // O->T connection ID
                                             to_id,           // T->O connection ID
                                             conn_serial,     // connection serial
                                             0x1234,          // vendor ID
                                             0x00000001),     // originator serial
                                 // Timeout multiplier + reserved + RPIs + params + transport
                                 struct_pack(conn->arena, "<B3xIHIHB",
                                             0x03,            // timeout multiplier
                                             0x00201340,      // O->T RPI (2s)
                                             0x43F4,          // O->T params (500 bytes, class 3)
                                             0x00201340,      // T->O RPI (2s)
                                             0x43F4,          // T->O params
                                             0xA3),           // transport trigger (class 3)
                                 // Connection path
                                 struct_pack(conn->arena, "<B*", (uint8_t)(conn_path.len / 2), &conn_path));

    // Wrap in Unconnected Send for proper routing to target device
    Bytes routed = create_unconnected_send(conn->arena, fo_data);
    Bytes response = eip_send_rr_data(conn->arena, conn, routed);

    if(response.len < 36) {  // Minimum: EIP header (24) + address (4) + data header (4) + CIP header (4)
        if(response.len >= 12) {
            uint32_t eip_status = 0;
            struct_unpack(bytes_slice(response, 8, 4), "<I", &eip_status);
            fprintf(stderr, "Forward Open response too short (got %zu bytes, EIP status: 0x%08X)\n", response.len, eip_status);
        } else {
            fprintf(stderr, "Forward Open response too short (got %zu bytes)\n", response.len);
        }
        return false;
    }

    CipResponse cip_resp = parse_cip_response(response);
    if(cip_resp.header.status != 0) {
        fprintf(stderr, "Forward Open failed with CIP status 0x%02X", cip_resp.header.status);
        if(cip_resp.header.ext_status_words > 0 && cip_resp.payload.len >= 2) {
            uint16_t ext_status = 0;
            struct_unpack(cip_resp.payload, "<H", &ext_status);
            fprintf(stderr, ", ext_status 0x%04X (%u)", ext_status, ext_status);
        }
        fprintf(stderr, "\n");
        return false;
    }

    // Extract O->T and T->O connection IDs from response
    // Response format: [reserved bytes][O->T ID:4][T->O ID:4][...]
    Bytes payload = cip_get_response_data(cip_resp);
    if(payload.data != NULL && payload.len >= 12) {
        uint32_t ot_connection_id = 0;
        uint32_t to_connection_id = 0;

        // Skip first 4 bytes (reserved), then read O->T and T->O IDs
        Bytes conn_data = bytes_slice(payload, 4, payload.len - 4);
        struct_unpack(conn_data, "<II", &ot_connection_id, &to_connection_id);

        printf("[*] Forward Open OK: O->T=0x%08X, T->O=0x%08X\n", ot_connection_id, to_connection_id);

        // Store connection info
        conn->cpid = ot_connection_id;
        conn->sssn = conn_serial;
        conn->connected = true;
        fprintf(stderr, "[DEBUG] eip_forward_open setting: connected=true, cpid=0x%08X, sssn=%u\n",
                conn->cpid, conn->sssn);
        fflush(stderr);

        // Sleep to allow PLC to fully establish the connection
        usleep(250000);  // 250ms

        return true;
    }

    fprintf(stderr, "Forward Open response payload too short (got %zu bytes)\n", payload.len);
    return false;
}

// Forward Close (0x4E) to close connected transport
void eip_forward_close(EipConnection *conn) {
    if(!conn->connected) {
        return;
    }

    // Build Forward Close request
    // CIP Request: Service, PathLen, Path (Connection Manager), Forward Close Data
    Bytes cip_req = struct_pack(conn->arena, "<BBBBBBBBHHIH",
                                0x4E, 2,           // service, path_len (2 words)
                                0x20, 0x06, 0x24, 0x01,  // cm_path (class 0x06, instance 0x01)
                                0x0A, 0x09,        // priority, timeout_ticks
                                conn->sssn,        // session serial number
                                0x01FA,            // vendor (Rockwell)
                                0x00000001,        // serial
                                0);                // reserved

    Bytes routed = create_unconnected_send(conn->arena, cip_req);
    eip_send_rr_data(conn->arena, conn, routed);

    conn->connected = false;
}

// Helper to send a CIP request through the full EIP stack
// Always uses Unconnected Send for now (connected transport format may not be supported)
Bytes send_cip_command(EipConnection *conn, Bytes cip_req) {
    fprintf(stderr, "[DEBUG] send_cip_command: connected=%d, cpid=0x%08X\n", conn->connected, conn->cpid);
    fflush(stderr);

    // If connected transport is established, use SendUnitData
    if(conn->connected && conn->cpid != 0) {
        fprintf(stderr, "[DEBUG] Using SendUnitData (connected)\n");
        fflush(stderr);
        return eip_send_unit_data(conn, cip_req);
    }

    fprintf(stderr, "[DEBUG] Using Unconnected Send\n");
    fflush(stderr);

    // Use Unconnected Send wrapper (Routing to Backplane 1, Slot 4)
    Bytes routed = create_unconnected_send(conn->arena, cip_req);
    Bytes packet = create_eip_packet(conn->arena, conn->session_handle, routed);

    bytes_hexdump(packet, "[SEND]");

    if(send(conn->sock_fd, packet.data, packet.len, 0) < 0) {
        perror("send failed");
        return (Bytes){NULL, 0};
    }

    // Reset arena after sending, allocate fresh space for response
    arena_reset(conn->arena);
    uint8_t *buf = arena_alloc(conn->arena, 4096);

    ssize_t received = recv(conn->sock_fd, buf, 4096, 0);
    if(received < 0) {
        perror("recv failed");
        return (Bytes){NULL, 0};
    }

    Bytes response = (Bytes){buf, (size_t)received};
    bytes_hexdump(response, "[RECV]");

    return response;
}

int main() {
    // Seed random number generator for Forward Open connection IDs
    srand((unsigned int)time(NULL));

    // 1. Setup Arena (1MB heap)
    Arena mem = arena_init(1024 * 1024);

    // Target Configuration
    const char *target_ip = "10.206.1.40";  // Hardcoded IP
    const char *tag_name = "TestBigArray";  // Hardcoded Tag
    // Path "1,4" is handled inside create_unconnected_send via 0x01, 0x04

    // Setup Signal Handler
    signal(SIGINT, sig_handler);

    printf("[*] Connecting to %s...\n", target_ip);
    EipConnection conn = eip_connect(&mem, target_ip, 44818);

    if(!eip_register_session(&conn)) {
        fprintf(stderr, "Failed to register session.\n");
        eip_disconnect(&conn);
        arena_free(&mem);
        return 1;
    }

    // ============================================================
    // Step 0: Open Forward Open connection
    // ============================================================
    printf("[*] Opening Forward Open connection...\n");
    if(!eip_forward_open(&conn, 4)) {
        fprintf(stderr, "Failed to open Forward Open connection.\n");
        eip_disconnect(&conn);
        arena_free(&mem);
        return 1;
    }

    fprintf(stderr, "[DEBUG] After Forward Open: connected=%d, cpid=0x%08X, sssn=%u\n",
            conn.connected, conn.cpid, conn.sssn);
    fflush(stderr);

    // ============================================================
    // Step 1: Read tag value and determine data type
    // ============================================================
    uint16_t tag_data_type = read_tag_value(&conn, tag_name);

    // ============================================================
    // Step 2: Create Trend Object (Service 0x08, Class 0xB2, Inst 0)
    // ============================================================
    printf("[*] Creating Trend Object...\n");
    Bytes create_path = create_cip_class_path(&mem, 0xB2, 0);
    Bytes create_payload = create_trend_payload(&mem, 0x1000, 1);  // buffer_size=4096 (0x1000), num_tags=1

    Bytes req_create = create_cip_request(&mem, 0x08, create_path, create_payload);
    Bytes resp_create = send_cip_command(&conn, req_create);

    // Parse Instance ID from response
    uint32_t trend_instance_id = 1;  // Default
    if(resp_create.len > 0) {
        CipResponse cip_resp = parse_cip_response(resp_create);
        if(cip_resp.header.status == 0) {
            Bytes payload = cip_get_response_data(cip_resp);
            if(payload.data != NULL && payload.len >= 4) {
                struct_unpack(payload, "<I", &trend_instance_id);
                printf("[*] Trend Created with Instance ID: %u\n", trend_instance_id);
            }
        } else {
            printf("[*] Create response status: 0x%02X (using default Instance ID: %u)\n", cip_resp.header.status, trend_instance_id);
        }
    }
    arena_reset(&mem);

    // ============================================================
    // Step 3: SetAttributeList (Service 0x04, Class 0xB2, Inst ID)
    // ============================================================
    printf("[*] Setting Attributes (sample_rate=100ms, state=0)...\n");
    Bytes attr_path = create_cip_class_path(&mem, 0xB2, trend_instance_id);
    Bytes attr_payload = create_set_attrs_payload(&mem, 10000, 0);  // 10000 us = 10ms, state=0

    Bytes req_attr = create_cip_request(&mem, 0x04, attr_path, attr_payload);
    Bytes resp_attr = send_cip_command(&conn, req_attr);
    if(resp_attr.len > 0) {
        CipResponse cip_resp = parse_cip_response(resp_attr);
        printf("[*] SetAttributes response status: 0x%02X\n", cip_resp.header.status);
    }
    arena_reset(&mem);

    // Verify attributes after setting
    get_trend_attributes(&conn, trend_instance_id);

    // ============================================================
    // Step 4: Add Tag to Trend (Service 0x4E, Class 0xB2, Inst ID)
    // ============================================================
    printf("[*] Adding Tag '%s' to Trend...\n", tag_name);
    Bytes add_path = create_cip_class_path(&mem, 0xB2, trend_instance_id);
    Bytes add_payload = create_add_tag_payload(&mem, tag_name);

    Bytes req_add = create_cip_request(&mem, 0x4E, add_path, add_payload);
    Bytes resp_add = send_cip_command(&conn, req_add);
    if(resp_add.len > 0) {
        CipResponse cip_resp = parse_cip_response(resp_add);
        printf("[*] AddTag response status: 0x%02X\n", cip_resp.header.status);
    }
    arena_reset(&mem);

    // ============================================================
    // Step 5: Start Trend (Service 0x06, Class 0xB2, Inst ID)
    // ============================================================
    printf("[*] Starting Trend...\n");
    Bytes start_path = create_cip_class_path(&mem, 0xB2, trend_instance_id);
    Bytes req_start = create_cip_request(&mem, 0x06, start_path, (Bytes){NULL, 0});
    Bytes resp_start = send_cip_command(&conn, req_start);
    if(resp_start.len > 0) {
        CipResponse cip_resp = parse_cip_response(resp_start);
        printf("[*] Start response status: 0x%02X\n", cip_resp.header.status);
    }
    arena_reset(&mem);

    // Verify attributes after starting
    get_trend_attributes(&conn, trend_instance_id);

    // ============================================================
    // Step 6: Read Loop (Service 0x4C, Class 0xB2, Inst ID)
    // ============================================================
    printf("[*] Starting Read Loop (Press Ctrl+C to stop)...\n");
    Bytes read_path = create_cip_class_path(&mem, 0xB2, trend_instance_id);
    Bytes req_read = create_cip_request(&mem, 0x4C, read_path, (Bytes){NULL, 0});

    int sample_count = 0;
    while(keep_running) {
        Bytes resp = send_cip_command(&conn, req_read);
        if(resp.len > 0) {
            CipResponse cip_resp = parse_cip_response(resp);
            if(cip_resp.header.status == 0) {
                Bytes samples = cip_get_response_data(cip_resp);
                if(samples.data != NULL && samples.len > 0) {
                    printf("[*] Sample batch %d:\n", ++sample_count);
                    parse_and_print_samples(samples, tag_data_type);
                }
            }
        }
        arena_reset(&mem);
        // Simple throttle
        usleep(100000);
    }
    printf("\n[*] Interrupted. Teardown...\n");

    // ============================================================
    // Step 7: Stop Trend (Service 0x07, Class 0xB2, Inst ID)
    // ============================================================
    if(trend_instance_id > 0) {
        printf("[*] Stopping Trend...\n");
        Bytes stop_path = create_cip_class_path(&mem, 0xB2, trend_instance_id);
        Bytes req_stop = create_cip_request(&mem, 0x07, stop_path, (Bytes){NULL, 0});
        Bytes resp_stop = send_cip_command(&conn, req_stop);
        if(resp_stop.len > 0) {
            CipResponse cip_resp = parse_cip_response(resp_stop);
            printf("[*] Stop response status: 0x%02X\n", cip_resp.header.status);
        }
        arena_reset(&mem);

        // ============================================================
        // Step 8: Remove Tag (Service 0x4F, Class 0xB2, Inst ID)
        // ============================================================
        printf("[*] Removing Tag...\n");
        Bytes remove_path = create_cip_class_path(&mem, 0xB2, trend_instance_id);
        Bytes remove_payload = create_remove_tag_payload(&mem, 1);  // tag_index=1

        Bytes req_remove = create_cip_request(&mem, 0x4F, remove_path, remove_payload);
        Bytes resp_remove = send_cip_command(&conn, req_remove);
        if(resp_remove.len > 0) {
            CipResponse cip_resp = parse_cip_response(resp_remove);
            printf("[*] RemoveTag response status: 0x%02X\n", cip_resp.header.status);
        }
        arena_reset(&mem);

        // ============================================================
        // Step 9: Delete Trend (Service 0x09, Class 0xB2, Inst ID)
        // ============================================================
        printf("[*] Deleting Trend Object...\n");
        Bytes delete_path = create_cip_class_path(&mem, 0xB2, trend_instance_id);
        Bytes req_delete = create_cip_request(&mem, 0x09, delete_path, (Bytes){NULL, 0});
        Bytes resp_delete = send_cip_command(&conn, req_delete);
        if(resp_delete.len > 0) {
            CipResponse cip_resp = parse_cip_response(resp_delete);
            printf("[*] Delete response status: 0x%02X\n", cip_resp.header.status);
        }
        arena_reset(&mem);
    }

    // Cleanup happens all at once
    eip_forward_close(&conn);
    eip_disconnect(&conn);
    arena_free(&mem);
    return 0;
}
