/*
 * test_send.cpp - Replicate exactly what mega65-tools etherload does.
 *
 * 1. Discover MEGA65 via beacon
 * 2. Send hyperrupt trigger to ff02::1
 * 3. Send echo ethlet to MEGA65 unicast address
 *
 * Usage: ./test_send
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

#define PORTNUM 4510

/* Hyperrupt trigger — exact copy from mega65-tools */
static unsigned char hyperrupt_trigger[128];
static unsigned char magic_string[12] = {
    0x65, 0x47, 0x53,       // 65 G S
    0x4b, 0x45, 0x59,       // KEY
    0x43, 0x4f, 0x44, 0x45, // CODE
    0x00, 0x80              // Magic key code $8000 = ethernet hypervisor trap
};

/* Simple ping payload: LDA #$00; INC $D020; RTS — padded to 1024 bytes */
static unsigned char ping_payload[1024];

int main()
{
    /* --- Step 1: Discover MEGA65 by listening for beacon --- */
    printf("Discovering MEGA65...\n");

    /* Find all IPv6 link-local interfaces */
    struct { sockaddr_in6 addr; unsigned scope_id; char name[32]; } ifaces[16];
    int num_ifaces = 0;

    ifaddrs* ifap = nullptr;
    getifaddrs(&ifap);
    for (auto* ifa = ifap; ifa && num_ifaces < 16; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        auto* sin6 = (sockaddr_in6*)ifa->ifa_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
        ifaces[num_ifaces].addr = *sin6;
        ifaces[num_ifaces].scope_id = if_nametoindex(ifa->ifa_name);
        strncpy(ifaces[num_ifaces].name, ifa->ifa_name, 31);
        num_ifaces++;
    }
    freeifaddrs(ifap);
    printf("Found %d IPv6 link-local interfaces\n", num_ifaces);

    /* Listen for "mega65" beacon on all interfaces */
    int discoverfd[16];
    for (int i = 0; i < num_ifaces; i++) {
        discoverfd[i] = socket(AF_INET6, SOCK_DGRAM, 0);
        fcntl(discoverfd[i], F_SETFL, O_NONBLOCK);
        int enable = 1;
        setsockopt(discoverfd[i], IPPROTO_IPV6, IPV6_V6ONLY, &enable, sizeof(enable));
        setsockopt(discoverfd[i], SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        setsockopt(discoverfd[i], SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));

        ipv6_mreq mreq{};
        mreq.ipv6mr_interface = ifaces[i].scope_id;
        inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
        setsockopt(discoverfd[i], IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq));

        sockaddr_in6 bind_addr{};
        bind_addr.sin6_family = AF_INET6;
        bind_addr.sin6_port = htons(PORTNUM);
        bind_addr.sin6_addr = in6addr_any;
        bind(discoverfd[i], (sockaddr*)&bind_addr, sizeof(bind_addr));
    }

    sockaddr_in6 servaddr{};
    servaddr.sin6_family = AF_INET6;
    servaddr.sin6_port = htons(PORTNUM);
    bool found = false;

    for (int attempt = 0; attempt < 5000 && !found; attempt++) {
        for (int i = 0; i < num_ifaces; i++) {
            char buf[256];
            sockaddr_in6 src{};
            socklen_t src_len = sizeof(src);
            ssize_t n = recvfrom(discoverfd[i], buf, sizeof(buf), 0,
                                 (sockaddr*)&src, &src_len);
            if (n == 6 && memcmp(buf, "mega65", 6) == 0) {
                memcpy(&servaddr.sin6_addr, &src.sin6_addr, sizeof(servaddr.sin6_addr));
                servaddr.sin6_scope_id = src.sin6_scope_id;
                char addr_str[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &src.sin6_addr, addr_str, sizeof(addr_str));
                char ifname[IF_NAMESIZE];
                if_indextoname(src.sin6_scope_id, ifname);
                printf("Found MEGA65 at %s%%%s (scope_id=%u)\n",
                       addr_str, ifname, src.sin6_scope_id);
                found = true;
                break;
            }
        }
        usleep(1000);
    }

    for (int i = 0; i < num_ifaces; i++)
        close(discoverfd[i]);

    if (!found) {
        printf("No MEGA65 found.\n");
        return 1;
    }

    /* --- Step 2: Create socket exactly like etherload --- */
    int sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    setsockopt(sockfd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
               &servaddr.sin6_scope_id, sizeof(servaddr.sin6_scope_id));
    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL) | O_NONBLOCK);

    sockaddr_in6 broadcast_addr{};
    broadcast_addr.sin6_family = AF_INET6;
    broadcast_addr.sin6_port = htons(PORTNUM);
    inet_pton(AF_INET6, "ff02::1", &broadcast_addr.sin6_addr);
    broadcast_addr.sin6_scope_id = servaddr.sin6_scope_id;

    /* --- Step 3: Send hyperrupt trigger (exactly like etherload) --- */
    memset(hyperrupt_trigger, 0, sizeof(hyperrupt_trigger));
    memcpy(&hyperrupt_trigger[0x24], magic_string, sizeof(magic_string));

    printf("Sending hyperrupt trigger to ff02::1...\n");
    ssize_t sent = sendto(sockfd, hyperrupt_trigger, sizeof(hyperrupt_trigger), 0,
                          (sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
    printf("  Sent %zd bytes (hyperrupt)\n", sent);
    usleep(10000);

    /* --- Step 4: Send ping to UNICAST address (like etherload sends data) --- */
    memset(ping_payload, 0, sizeof(ping_payload));
    ping_payload[0] = 0xa9;  /* LDA #$00 */
    ping_payload[1] = 0x00;
    ping_payload[2] = 0xee;  /* INC $D020 */
    ping_payload[3] = 0x20;
    ping_payload[4] = 0xd0;
    ping_payload[5] = 0x60;  /* RTS */

    printf("Sending 1024-byte ping to MEGA65 unicast...\n");
    for (int i = 0; i < 10; i++) {
        ssize_t r;
        do {
            r = sendto(sockfd, ping_payload, 1024, 0,
                       (sockaddr*)&servaddr, sizeof(servaddr));
        } while (r < 0 && errno == EAGAIN);
        printf("  Attempt %d: sent %zd bytes (errno=%d)\n", i + 1, r, r < 0 ? errno : 0);
        usleep(200000);
    }

    /* --- Step 5: Also try sending to MULTICAST --- */
    printf("\nAlso sending ping to ff02::1 multicast...\n");
    for (int i = 0; i < 5; i++) {
        sent = sendto(sockfd, ping_payload, 1024, 0,
                      (sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
        printf("  Attempt %d: sent %zd bytes\n", i + 1, sent);
        usleep(200000);
    }

    printf("\nDid the MEGA65 border colour change?\n");
    close(sockfd);
    return 0;
}
