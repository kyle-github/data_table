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

volatile sig_atomic_t keep_running = 1;

void sig_handler(int _) {
    (void)_;
    keep_running = 0;
}

// ==========================================
// EIP CONNECTION
// ==========================================

typedef struct {
    int sock_fd;
    uint32_t session_handle;
    uint8_t slot;  // target slot for routing
    // Connected transport state
    bool connected;
    uint32_t cpid;               // O->T connection ID (target-assigned)
    uint16_t conn_serial;        // connection serial number
    uint16_t vendor_id;          // originator vendor ID
    uint32_t originator_serial;  // originator serial number
    uint16_t sequence;           // sequence number for connected messages
} EipConnection;

// Forward declarations
Bytes send_cip_command(EipConnection *conn, Arena *a, Bytes cip_req);
Bytes eip_send_receive(EipConnection *conn, Arena *a, Bytes packet);

// ==========================================
// TAG / TREND HELPERS
// ==========================================

uint16_t read_tag_value(EipConnection *conn, Arena *a, const char *tag_name) {
    printf("[*] Reading tag '%s'...\n", tag_name);

    Bytes sym_path = encode_tag_name(a, tag_name);
    uint8_t path_words = (uint8_t)(sym_path.len / 2);
    Bytes cip_req = bytes_pack(a, "<BB*H", 0x4C, path_words, &sym_path, (uint16_t)1);

    Bytes response = send_cip_command(conn, a, cip_req);

    uint16_t data_type = 0;
    if(response.len > 0) {
        CipResponse cip_resp = eip_parse_response(response);
        if(cip_resp.header.status == 0) {
            Bytes payload = cip_get_response_data(cip_resp);
            if(payload.data != NULL && payload.len >= 6) {
                uint32_t value_raw = 0;
                bytes_unpack(payload, "<HI", &data_type, &value_raw);

                const char *type_name = "UNKNOWN";
                if(data_type == 0xC1) {
                    type_name = "BOOL";
                } else if(data_type == 0xC2) {
                    type_name = "SINT";
                } else if(data_type == 0xC3) {
                    type_name = "INT";
                } else if(data_type == 0xC4) {
                    type_name = "DINT";
                } else if(data_type == 0xCA) {
                    type_name = "REAL";
                }

                if(data_type == 0xCA) {
                    float val = *(float *)&value_raw;
                    printf("  %s = %.4f\n", type_name, val);
                } else {
                    int32_t val = (int32_t)value_raw;
                    printf("  %s = %d\n", type_name, val);
                }
            }
        } else {
            printf("  Read failed: status=0x%02X\n", cip_resp.header.status);
        }
    }
    arena_reset(a);
    return data_type;
}

void get_trend_attributes(EipConnection *conn, Arena *a, uint32_t instance_id) {
    printf("[*] Getting Trend Attributes for instance %u...\n", instance_id);

    Bytes attr_data = bytes_pack(a, "<HHHHHHHH", 7, 1, 3, 5, 6, 7, 8, 0x0A);
    Bytes cip_req = cip_encode_object_service(a, 0x03, 0xB2, (uint16_t)instance_id, attr_data);

    Bytes response = send_cip_command(conn, a, cip_req);

    if(response.len > 0) {
        CipResponse cip_resp = eip_parse_response(response);
        printf("  Status: 0x%02X\n", cip_resp.header.status);
        if(cip_resp.header.status == 0) {
            Bytes payload = cip_get_response_data(cip_resp);
            if(payload.data != NULL && payload.len >= 2) {
                uint16_t attr_count_resp = 0;
                bytes_unpack(payload, "<H", &attr_count_resp);
                printf("  Attributes: %u\n", attr_count_resp);
            }
        }
    }
    arena_reset(a);
}

// ==========================================
// EIP CONNECTION FUNCTIONS
// ==========================================

EipConnection eip_connect(const char *ip, int port, uint8_t slot) {
    EipConnection conn = {0};
    conn.sock_fd = -1;
    conn.slot = slot;

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
        fprintf(stderr, "Invalid address\n");
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

void eip_disconnect(EipConnection *conn) {
    if(conn && conn->sock_fd >= 0) {
        close(conn->sock_fd);
        conn->sock_fd = -1;
    }
}

// Send a packet and receive the response
Bytes eip_send_receive(EipConnection *conn, Arena *a, Bytes packet) {
    bytes_hexdump(packet, "[SEND]");

    if(send(conn->sock_fd, packet.data, packet.len, 0) < 0) {
        perror("send failed");
        return (Bytes){NULL, 0};
    }

    arena_reset(a);
    uint8_t *buf = arena_alloc(a, 4096);

    ssize_t received = recv(conn->sock_fd, buf, 4096, 0);
    if(received < 0) {
        perror("recv failed");
        return (Bytes){NULL, 0};
    }

    Bytes response = (Bytes){buf, (size_t)received};
    bytes_hexdump(response, "[RECV]");
    return response;
}

bool eip_register_session(EipConnection *conn, Arena *a) {
    Bytes reg_payload = bytes_pack(a, "<HH", 1, 0);  // protocol, flags
    Bytes packet = eip_encode_header(a, EIP_CMD_REGISTER_SESSION, 0, reg_payload);

    if(send(conn->sock_fd, packet.data, packet.len, 0) < 0) {
        perror("RegisterSession send failed");
        return false;
    }

    arena_reset(a);
    uint8_t *buf = arena_alloc(a, 256);
    ssize_t received = recv(conn->sock_fd, buf, 256, 0);
    if(received < 24) {
        fprintf(stderr, "RegisterSession recv failed or got truncated response\n");
        return false;
    }

    uint32_t eip_status = 0;
    bytes_unpack(bytes_slice(bytes_from_buf(buf, received), 8, received - 8), "<I", &eip_status);
    if(eip_status != 0) {
        fprintf(stderr, "RegisterSession failed with EIP status: 0x%08X\n", eip_status);
        return false;
    }

    bytes_unpack(bytes_slice(bytes_from_buf(buf, received), 4, received - 4), "<I", &conn->session_handle);
    printf("[*] Session registered with handle: 0x%08X\n", conn->session_handle);
    return true;
}

// Forward Open — sent directly to Connection Manager (not wrapped in Unconnected Send)
bool eip_forward_open(EipConnection *conn, Arena *a) {
    uint32_t ot_id = 0x80000000 | (rand() & 0xFFFF);
    uint32_t to_id = 0x803F0000 | (rand() & 0xFFFF);
    uint16_t conn_serial = (uint16_t)(rand() & 0xFFFF);

    conn->vendor_id = 0xF33D;
    conn->originator_serial = 0x21504345;  // "!PCE"

    ForwardOpenParams fo_params = {
        .ot_connection_id = ot_id,
        .to_connection_id = to_id,
        .connection_serial = conn_serial,
        .vendor_id = conn->vendor_id,
        .originator_serial = conn->originator_serial,
        .timeout_multiplier = 0x03,
        .ot_rpi = 0x00201340,  // ~2s
        .ot_params = 0x43F4,   // 500 bytes, class 3
        .to_rpi = 0x00201340,
        .to_params = 0x43F4,
        .transport_trigger = 0xA3,  // class 3
    };

    // Route to the end device's Message Router: backplane port 1, slot N, class 2, instance 1
    Bytes mr_route = cip_encode_mr_route(a, 0x01, conn->slot);

    // Forward Open payload (parameters + connection path to Message Router)
    Bytes fo_payload = cip_encode_forward_open_payload(a, fo_params, mr_route);

    // Forward Open service (0x54) addressed to Connection Manager (class 0x06, instance 0x01)
    Bytes fo_request = cip_encode_object_service(a, 0x54, 0x06, 0x01, fo_payload);

    // Wrap in CPF unconnected + EIP header — NO Unconnected Send wrapper
    Bytes cpf = cpf_encode_unconnected(a, fo_request);
    Bytes packet = eip_encode_header(a, EIP_CMD_SEND_RR_DATA, conn->session_handle, cpf);

    Bytes response = eip_send_receive(conn, a, packet);

    if(response.len < 36) {
        fprintf(stderr, "Forward Open response too short (got %zu bytes)\n", response.len);
        return false;
    }

    CipResponse cip_resp = eip_parse_response(response);
    if(cip_resp.header.status != 0) {
        fprintf(stderr, "Forward Open failed with CIP status 0x%02X", cip_resp.header.status);
        if(cip_resp.header.ext_status_words > 0 && cip_resp.payload.len >= 2) {
            uint16_t ext_status = 0;
            bytes_unpack(cip_resp.payload, "<H", &ext_status);
            fprintf(stderr, ", ext_status 0x%04X (%u)", ext_status, ext_status);
        }
        fprintf(stderr, "\n");
        return false;
    }

    ForwardOpenResponse fo_resp = cip_parse_forward_open_response(cip_resp);
    if(!fo_resp.valid) {
        fprintf(stderr, "Forward Open response payload invalid\n");
        return false;
    }

    printf("[*] Forward Open OK: O->T=0x%08X, T->O=0x%08X\n", fo_resp.ot_connection_id, fo_resp.to_connection_id);

    conn->cpid = fo_resp.ot_connection_id;
    conn->conn_serial = conn_serial;
    conn->connected = true;

    usleep(250000);  // 250ms settle time
    return true;
}

// Forward Close — sent directly to Connection Manager (not wrapped in Unconnected Send)
void eip_forward_close(EipConnection *conn, Arena *a) {
    if(!conn->connected) { return; }

    Bytes mr_route = cip_encode_mr_route(a, 0x01, conn->slot);
    Bytes fc_payload = cip_encode_forward_close_payload(a, conn->conn_serial, conn->vendor_id, conn->originator_serial, mr_route);

    // Forward Close service (0x4E) to Connection Manager (class 0x06, instance 0x01)
    Bytes fc_request = cip_encode_object_service(a, 0x4E, 0x06, 0x01, fc_payload);

    Bytes cpf = cpf_encode_unconnected(a, fc_request);
    Bytes packet = eip_encode_header(a, EIP_CMD_SEND_RR_DATA, conn->session_handle, cpf);

    eip_send_receive(conn, a, packet);
    conn->connected = false;
}

// ==========================================
// SEND CIP COMMAND (connected or unconnected)
// ==========================================

Bytes send_cip_command(EipConnection *conn, Arena *a, Bytes cip_req) {
    if(conn->connected && conn->cpid != 0) {
        // Connected: SendUnitData (0x70)
        conn->sequence++;
        Bytes cpf = cpf_encode_connected(a, conn->cpid, conn->sequence, cip_req);
        Bytes packet = eip_encode_header(a, EIP_CMD_SEND_UNIT_DATA, conn->session_handle, cpf);
        return eip_send_receive(conn, a, packet);
    }

    // Unconnected: wrap in Unconnected Send (0x52) with route, then SendRRData
    Bytes route = cip_encode_port_segment(a, 0x01, conn->slot);
    Bytes uc = cip_encode_unconnected(a, cip_req, route);
    Bytes cpf = cpf_encode_unconnected(a, uc);
    Bytes packet = eip_encode_header(a, EIP_CMD_SEND_RR_DATA, conn->session_handle, cpf);
    return eip_send_receive(conn, a, packet);
}

// ==========================================
// MAIN
// ==========================================

int main() {
    srand((unsigned int)time(NULL));

    Arena mem = arena_init(1024 * 1024);

    const char *target_ip = "10.206.1.40";
    const char *tag_name = "TestBigArray";
    uint8_t target_slot = 4;

    signal(SIGINT, sig_handler);

    printf("[*] Connecting to %s...\n", target_ip);
    EipConnection conn = eip_connect(target_ip, 44818, target_slot);

    if(!eip_register_session(&conn, &mem)) {
        fprintf(stderr, "Failed to register session.\n");
        eip_disconnect(&conn);
        printf("[arena] high-water: %zu / %zu bytes\n", mem.high_water, mem.capacity);
        arena_free(&mem);
        return 1;
    }
    printf("[arena] after register_session: high-water=%zu\n", mem.high_water);

    // Step 0: Forward Open
    printf("[*] Opening Forward Open connection...\n");
    if(!eip_forward_open(&conn, &mem)) {
        fprintf(stderr, "Failed to open Forward Open connection.\n");
        eip_disconnect(&conn);
        printf("[arena] high-water: %zu / %zu bytes\n", mem.high_water, mem.capacity);
        arena_free(&mem);
        return 1;
    }
    printf("[arena] after forward_open: high-water=%zu\n", mem.high_water);

    // Step 1: Read tag value and determine data type
    uint16_t tag_data_type = read_tag_value(&conn, &mem, tag_name);
    printf("[arena] after read_tag_value: high-water=%zu\n", mem.high_water);

    // Step 2: Create Trend Object (Service 0x08, Class 0xB2, Inst 0)
    printf("[*] Creating Trend Object...\n");
    Bytes create_payload = create_trend_payload(&mem, 0x1000, 1);
    Bytes req_create = cip_encode_object_service(&mem, 0x08, 0xB2, 0, create_payload);
    Bytes resp_create = send_cip_command(&conn, &mem, req_create);

    uint32_t trend_instance_id = 1;
    if(resp_create.len > 0) {
        CipResponse cip_resp = eip_parse_response(resp_create);
        if(cip_resp.header.status == 0) {
            Bytes payload = cip_get_response_data(cip_resp);
            if(payload.data != NULL && payload.len >= 4) {
                bytes_unpack(payload, "<I", &trend_instance_id);
                printf("[*] Trend Created with Instance ID: %u\n", trend_instance_id);
            }
        } else {
            printf("[*] Create response status: 0x%02X (using default Instance ID: %u)\n", cip_resp.header.status,
                   trend_instance_id);
        }
    }
    arena_reset(&mem);
    printf("[arena] after create_trend: high-water=%zu\n", mem.high_water);

    // Step 3: SetAttributeList (Service 0x04, Class 0xB2, Inst ID)
    printf("[*] Setting Attributes (sample_rate=10ms, state=0)...\n");
    Bytes attr_payload = create_set_attrs_payload(&mem, 10000, 0);
    Bytes req_attr = cip_encode_object_service(&mem, 0x04, 0xB2, (uint16_t)trend_instance_id, attr_payload);
    Bytes resp_attr = send_cip_command(&conn, &mem, req_attr);
    if(resp_attr.len > 0) {
        CipResponse cip_resp = eip_parse_response(resp_attr);
        printf("[*] SetAttributes response status: 0x%02X\n", cip_resp.header.status);
    }
    arena_reset(&mem);
    printf("[arena] after set_attrs: high-water=%zu\n", mem.high_water);

    get_trend_attributes(&conn, &mem, trend_instance_id);

    // Step 4: Add Tag to Trend (Service 0x4E, Class 0xB2, Inst ID)
    printf("[*] Adding Tag '%s' to Trend...\n", tag_name);
    Bytes add_payload = create_add_tag_payload(&mem, tag_name);
    Bytes req_add = cip_encode_object_service(&mem, 0x4E, 0xB2, (uint16_t)trend_instance_id, add_payload);
    Bytes resp_add = send_cip_command(&conn, &mem, req_add);
    if(resp_add.len > 0) {
        CipResponse cip_resp = eip_parse_response(resp_add);
        printf("[*] AddTag response status: 0x%02X\n", cip_resp.header.status);
    }
    arena_reset(&mem);
    printf("[arena] after add_tag: high-water=%zu\n", mem.high_water);

    // Step 5: Start Trend (Service 0x06, Class 0xB2, Inst ID)
    printf("[*] Starting Trend...\n");
    Bytes req_start = cip_encode_object_service(&mem, 0x06, 0xB2, (uint16_t)trend_instance_id, (Bytes){NULL, 0});
    Bytes resp_start = send_cip_command(&conn, &mem, req_start);
    if(resp_start.len > 0) {
        CipResponse cip_resp = eip_parse_response(resp_start);
        printf("[*] Start response status: 0x%02X\n", cip_resp.header.status);
    }
    arena_reset(&mem);
    printf("[arena] after start_trend: high-water=%zu\n", mem.high_water);

    get_trend_attributes(&conn, &mem, trend_instance_id);

    // Step 6: Read Loop (Service 0x4C, Class 0xB2, Inst ID)
    printf("[*] Starting Read Loop (Press Ctrl+C to stop)...\n");

    int sample_count = 0;
    while(keep_running) {
        Bytes req_read = cip_encode_object_service(&mem, 0x4C, 0xB2, (uint16_t)trend_instance_id, (Bytes){NULL, 0});
        Bytes resp = send_cip_command(&conn, &mem, req_read);
        if(resp.len > 0) {
            CipResponse cip_resp = eip_parse_response(resp);
            if(cip_resp.header.status == 0) {
                Bytes samples = cip_get_response_data(cip_resp);
                if(samples.data != NULL && samples.len > 0) {
                    printf("[*] Sample batch %d:\n", ++sample_count);
                    parse_and_print_samples(samples, tag_data_type);
                }
            }
        }
        arena_reset(&mem);
        usleep(100000);
    }
    printf("\n[*] Interrupted. Teardown...\n");

    // Step 7: Stop Trend
    if(trend_instance_id > 0) {
        printf("[*] Stopping Trend...\n");
        Bytes req_stop = cip_encode_object_service(&mem, 0x07, 0xB2, (uint16_t)trend_instance_id, (Bytes){NULL, 0});
        Bytes resp_stop = send_cip_command(&conn, &mem, req_stop);
        if(resp_stop.len > 0) {
            CipResponse cip_resp = eip_parse_response(resp_stop);
            printf("[*] Stop response status: 0x%02X\n", cip_resp.header.status);
        }
        arena_reset(&mem);

        // Step 8: Remove Tag
        printf("[*] Removing Tag...\n");
        Bytes remove_payload = create_remove_tag_payload(&mem, 1);
        Bytes req_remove = cip_encode_object_service(&mem, 0x4F, 0xB2, (uint16_t)trend_instance_id, remove_payload);
        Bytes resp_remove = send_cip_command(&conn, &mem, req_remove);
        if(resp_remove.len > 0) {
            CipResponse cip_resp = eip_parse_response(resp_remove);
            printf("[*] RemoveTag response status: 0x%02X\n", cip_resp.header.status);
        }
        arena_reset(&mem);

        // Step 9: Delete Trend
        printf("[*] Deleting Trend Object...\n");
        Bytes req_delete = cip_encode_object_service(&mem, 0x09, 0xB2, (uint16_t)trend_instance_id, (Bytes){NULL, 0});
        Bytes resp_delete = send_cip_command(&conn, &mem, req_delete);
        if(resp_delete.len > 0) {
            CipResponse cip_resp = eip_parse_response(resp_delete);
            printf("[*] Delete response status: 0x%02X\n", cip_resp.header.status);
        }
        arena_reset(&mem);
    }

    eip_forward_close(&conn, &mem);
    eip_disconnect(&conn);
    printf("[arena] high-water: %zu / %zu bytes\n", mem.high_water, mem.capacity);
    arena_free(&mem);
    return 0;
}
