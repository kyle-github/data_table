#include "arena.h"
#include "bytes.h"
#include "cip.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
    uint32_t cpid;
    uint16_t sssn;
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
    conn.sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(conn.sock_fd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if(inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address/ Address not supported \n");
        close(conn.sock_fd);
        exit(1);
    }

    if(connect(conn.sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed");
        close(conn.sock_fd);
        exit(1);
    }

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

    return (Bytes){buf, (size_t)received};
}

// Forward Open (0x54) to establish connected transport
bool eip_forward_open(EipConnection *conn, uint8_t slot) {
    // Build Forward Open request
    // CIP Request: Service, PathLen, Path (Connection Manager), Forward Open Data
    Bytes cip_req = struct_pack(conn->arena, "<BBBBBBBBIHHIHIB",
                                0x54, 2,           // service, path_len (2 words)
                                0x20, 0x06, 0x24, 0x01,  // cm_path (class 0x06, instance 0x01)
                                0x0A, 0x09,        // priority, timeout_ticks
                                0,                 // cpid (assigned by PLC)
                                0x0001, 0,         // sssn, reserved
                                30000000,          // conn_timeout (30s in µs)
                                0x01FA,            // vendor (Rockwell)
                                0x00000001,        // serial
                                0);                // conn_path_size

    // Wrap in Unconnected Send and send
    Bytes routed = create_unconnected_send(conn->arena, cip_req);
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
        fprintf(stderr, "Forward Open failed with status 0x%02X\n", cip_resp.header.status);
        return false;
    }

    // Extract CPID and serial from response
    Bytes payload = cip_get_response_data(cip_resp);
    if(payload.data != NULL && payload.len >= 6) {
        struct_unpack(payload, "<IH", &conn->cpid, &conn->sssn);
        conn->connected = true;
        printf("[*] Forward Open succeeded: CPID=0x%08X SSSN=0x%04X\n", conn->cpid, conn->sssn);
        return true;
    }

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

// Helper to send a CIP request through the full EIP stack (using Unconnected Send)
Bytes send_cip_command(EipConnection *conn, Bytes cip_req) {
    // 1. Wrap in Unconnected Send (Routing to Backplane 1, Slot 4)
    Bytes routed = create_unconnected_send(conn->arena, cip_req);

    // 2. Send via SendRRData
    Bytes response = eip_send_rr_data(conn->arena, conn, routed);

    return response;
}

int main() {
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
