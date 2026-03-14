#include "arena.h"
#include "bytes.h"
#include "cip.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define EIP_PORT 44818
#define MAX_RESPONSE_DELAY_MS 2000
#define RECV_TIMEOUT_SEC 3

// Parse "w.x.y.z/p" into broadcast address and local interface address.
// Returns true on success.
static bool parse_cidr_and_bind(const char *cidr, struct in_addr *broadcast_out, struct in_addr *local_out) {
    char ip_part[16];
    const char *slash = strchr(cidr, '/');
    if(!slash) {
        fprintf(stderr, "CIDR must contain '/' (e.g. 10.206.1.0/24)\n");
        return false;
    }

    size_t ip_len = (size_t)(slash - cidr);
    if(ip_len >= sizeof(ip_part)) {
        fprintf(stderr, "IP address too long\n");
        return false;
    }
    memcpy(ip_part, cidr, ip_len);
    ip_part[ip_len] = '\0';

    struct in_addr addr;
    if(inet_pton(AF_INET, ip_part, &addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", ip_part);
        return false;
    }

    unsigned long prefix = strtoul(slash + 1, NULL, 10);
    if(prefix > 32) {
        fprintf(stderr, "Prefix length must be 0-32, got %lu\n", prefix);
        return false;
    }

    uint32_t ip_host = ntohl(addr.s_addr);
    uint32_t netmask = (prefix == 0) ? 0 : (0xFFFFFFFFU << (32 - prefix));
    uint32_t network_base = ip_host & netmask;
    uint32_t broadcast = network_base | (~netmask);

    broadcast_out->s_addr = htonl(broadcast);

    // Find a local interface on this network
    struct ifaddrs *ifaddr_list = NULL;
    if(getifaddrs(&ifaddr_list) == -1) {
        perror("getifaddrs");
        return false;
    }

    bool found = false;
    for(struct ifaddrs *ifa = ifaddr_list; ifa != NULL; ifa = ifa->ifa_next) {
        if(ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) { continue; }
        uint32_t iface_ip = ntohl(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr);
        if((iface_ip & netmask) == network_base) {
            *local_out = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
            found = true;
            break;
        }
    }
    freeifaddrs(ifaddr_list);

    if(!found) {
        fprintf(stderr, "No local interface found on network %s\n", cidr);
        return false;
    }
    return true;
}

static void parse_identity(Bytes data) {
    if(data.len < 24) {
        printf("  Response too short (%zu bytes)\n", data.len);
        return;
    }

    // EIP Header: Command(2) Length(2) Session(4) Status(4) Context(8) Options(4)
    uint16_t eip_cmd = 0, eip_len = 0;
    uint32_t eip_session = 0, eip_status = 0, eip_options = 0;
    uint64_t eip_context = 0;
    Bytes remaining = bytes_unpack(data, "<HHIIQI", &eip_cmd, &eip_len, &eip_session, &eip_status, &eip_context, &eip_options);

    if(remaining.data == NULL || remaining.len < 2) {
        printf("  No CPF data in response\n");
        return;
    }

    // CPF: ItemCount(2)
    uint16_t item_count = 0;
    remaining = bytes_unpack(remaining, "<H", &item_count);
    if(item_count < 1 || remaining.data == NULL) {
        printf("  No CPF items\n");
        return;
    }

    // CPF Item: TypeID(2) Length(2) Data(Length)
    uint16_t item_type = 0, item_len = 0;
    remaining = bytes_unpack(remaining, "<HH", &item_type, &item_len);
    if(remaining.data == NULL || remaining.len < item_len) {
        printf("  CPF item truncated\n");
        return;
    }

    // Identity item (type 0x000C): EIP version(2) + sockaddr(16) + identity fields
    if(item_type != 0x000C) {
        printf("  Unexpected CPF item type: 0x%04X\n", item_type);
        return;
    }

    Bytes identity = bytes_slice(remaining, 0, item_len);

    // Encapsulation protocol version (2 bytes)
    uint16_t encap_version = 0;
    identity = bytes_unpack(identity, "<H", &encap_version);

    // Socket address: sin_family(2 big) sin_port(2 big) sin_addr(4 big) sin_zero(8)
    if(identity.data == NULL || identity.len < 16) {
        printf("  Socket address truncated\n");
        return;
    }
    uint16_t sin_family = 0, sin_port = 0;
    uint32_t sin_addr = 0;
    // Socket address fields are big-endian
    identity = bytes_unpack(identity, ">HHI8x", &sin_family, &sin_port, &sin_addr);

    char ip_str[INET_ADDRSTRLEN];
    uint32_t addr_net = htonl(sin_addr);
    inet_ntop(AF_INET, &addr_net, ip_str, sizeof(ip_str));

    if(identity.data == NULL || identity.len < 14) {
        printf("  Identity data truncated after sockaddr\n");
        return;
    }

    // Vendor ID(2) Device Type(2) Product Code(2) Revision(1.1) Status(2) Serial(4)
    uint16_t vendor_id = 0, device_type = 0, product_code = 0, status_word = 0;
    uint8_t rev_major = 0, rev_minor = 0;
    uint32_t serial_number = 0;
    identity = bytes_unpack(identity, "<HHHBBHI", &vendor_id, &device_type, &product_code, &rev_major, &rev_minor, &status_word,
                            &serial_number);

    if(identity.data == NULL || identity.len < 1) {
        printf("  Identity data truncated before product name\n");
        return;
    }

    // Product Name: SHORT_STRING (1 byte length + N bytes)
    uint8_t name_len = 0;
    identity = bytes_unpack(identity, "<B", &name_len);

    char product_name[256] = {0};
    if(identity.data != NULL && identity.len >= name_len && name_len > 0) {
        size_t copy_len = name_len < 255 ? name_len : 255;
        memcpy(product_name, identity.data, copy_len);
        product_name[copy_len] = '\0';
        identity = bytes_slice(identity, name_len, identity.len - name_len);
    }

    // State (1 byte) - optional
    uint8_t state = 0;
    if(identity.data != NULL && identity.len >= 1) { bytes_unpack(identity, "<B", &state); }

    printf("  Address:      %s:%u\n", ip_str, sin_port);
    printf("  Vendor ID:    %u (0x%04X)\n", vendor_id, vendor_id);
    printf("  Device Type:  %u (0x%04X)\n", device_type, device_type);
    printf("  Product Code: %u (0x%04X)\n", product_code, product_code);
    printf("  Revision:     %u.%u\n", rev_major, rev_minor);
    printf("  Status:       0x%04X\n", status_word);
    printf("  Serial:       0x%08X\n", serial_number);
    printf("  Product Name: %s\n", product_name);
    printf("  State:        %u\n", state);
}

int main(int argc, char *argv[]) {
    const char *cidr = "10.206.1.0/24";
    if(argc > 1) { cidr = argv[1]; }

    struct in_addr broadcast_addr, local_addr;
    if(!parse_cidr_and_bind(cidr, &broadcast_addr, &local_addr)) { return 1; }

    char bcast_str[INET_ADDRSTRLEN], local_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &broadcast_addr, bcast_str, sizeof(bcast_str));
    inet_ntop(AF_INET, &local_addr, local_str, sizeof(local_str));

    Arena mem = arena_init(64 * 1024);

    // Build ListIdentity EIP packet with max response delay in sender context
    // Header: Command(2) Length(2) Session(4) Status(4) Context(8) Options(4)
    // Context low 16 bits = max delay in ms for device response spreading
    Bytes packet = bytes_pack(&mem, "<HHIIH6xI", (uint16_t)EIP_CMD_LIST_IDENTITY,
                              (uint16_t)0,                      // length (no payload)
                              (uint32_t)0,                      // session
                              (uint32_t)0,                      // status
                              (uint16_t)MAX_RESPONSE_DELAY_MS,  // context low 16 bits
                              (uint32_t)0);                     // options

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0) {
        perror("socket");
        arena_free(&mem);
        return 1;
    }

    // Enable broadcast
    int broadcast_en = 1;
    if(setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_en, sizeof(broadcast_en)) < 0) {
        perror("setsockopt SO_BROADCAST");
        close(sock);
        arena_free(&mem);
        return 1;
    }

    // Set receive timeout
    struct timeval tv = {.tv_sec = RECV_TIMEOUT_SEC, .tv_usec = 0};
    if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sock);
        arena_free(&mem);
        return 1;
    }

    // Bind to the local interface on this network
    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr = local_addr;
    bind_addr.sin_port = 0;  // ephemeral port
    if(bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(sock);
        arena_free(&mem);
        return 1;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(EIP_PORT);
    dest.sin_addr = broadcast_addr;

    printf("[*] Network: %s, Local: %s, Broadcast: %s:%d\n", cidr, local_str, bcast_str, EIP_PORT);
    bytes_hexdump(packet, "[SEND]");

    if(sendto(sock, packet.data, packet.len, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        perror("sendto");
        close(sock);
        arena_free(&mem);
        return 1;
    }

    arena_reset(&mem);

    // Receive responses until timeout
    int count = 0;
    for(;;) {
        uint8_t *buf = arena_alloc(&mem, 4096);
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);

        ssize_t n = recvfrom(sock, buf, 4096, 0, (struct sockaddr *)&from, &from_len);
        if(n <= 0) { break; }

        char from_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));

        Bytes response = bytes_from_buf(buf, (size_t)n);
        count++;
        printf("\n[%d] Response from %s (%zd bytes):\n", count, from_ip, n);
        bytes_hexdump(response, "[RECV]");
        parse_identity(response);

        arena_reset(&mem);
    }

    printf("\n[*] %d device(s) found.\n", count);

    close(sock);
    printf("[arena] high-water: %zu / %zu bytes\n", mem.high_water, mem.capacity);
    arena_free(&mem);
    return 0;
}
