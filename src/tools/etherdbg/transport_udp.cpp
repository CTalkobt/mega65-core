/*
 * transport_udp.cpp - IPv6 UDP transport for MEGA65 Ethernet debug
 *
 * The MEGA65's ETHLOAD.M65 listener uses IPv6 link-local addressing
 * and UDP port 4510. Discovery works by listening for the "mega65"
 * beacon that the MEGA65 broadcasts when ETHLOAD is active.
 *
 * Protocol based on mega65-tools/src/tools/etherload/etherload_common.c
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstring>
#include <print>
#include <string>
#include <vector>

#include "transport_udp.h"
#include "protocol.h"

namespace etherdbg {

// ---------------------------------------------------------------------------
// UdpTransport - IPv6 UDP implementation
// ---------------------------------------------------------------------------

/* ETHLOAD requires packets to be 1024 bytes (padded with zeros) */
static constexpr size_t ETHLOAD_PACKET_SIZE = 1024;

class UdpTransport final : public Transport {
public:
    UdpTransport(int sockfd, sockaddr_in6 dest, sockaddr_in6 bcast)
        : sockfd_(sockfd), dest_(dest), broadcast_(bcast) {}

    ~UdpTransport() override {
#ifdef _WIN32
        closesocket(sockfd_);
        WSACleanup();
#else
        ::close(sockfd_);
#endif
    }

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    /* Send the hyperrupt trigger to ff02::1 to activate ETHLOAD.
     * This resolves NDP as a side effect (ETHLOAD sends beacons
     * which populate the neighbor table). */
    void activate() override {
        send_hyperrupt();
    }

    /* After activation, switch to multicast for all sends.
     * NDP entries decay quickly and unicast will stop working. */
    void use_multicast_for_sends() {
        dest_ = broadcast_;
    }

    /* Send the hyperrupt trigger to activate ETHLOAD */
    void send_hyperrupt() {
        static const uint8_t magic[] = {
            0x65, 0x47, 0x53,       // eGS
            0x4b, 0x45, 0x59,       // KEY
            0x43, 0x4f, 0x44, 0x45, // CODE
            0x00, 0x80              // $8000 = ethernet hypervisor trap
        };
        uint8_t trigger[128] = {};
        std::memcpy(&trigger[0x24], magic, sizeof(magic));
        sendto(sockfd_, trigger, sizeof(trigger), 0,
               reinterpret_cast<const sockaddr*>(&broadcast_),
               sizeof(broadcast_));
        usleep(10000);
    }

    /*
     * Send the echo ethlet repeatedly to the unicast address until
     * ETHLOAD responds with a copy of the packet (ACK).
     * This matches ethl_ping() from mega65-tools.
     */
    bool wait_for_ethload(int timeout_ms) {
        /* ethlet_echo: the exact ping payload from mega65-tools */
        static const uint8_t ethlet_echo[] = {
            0xa9,0x00,0xa9,0x47,0x8d,0x2f,0xd0,0xa9,0x53,0x8d,0x2f,0xd0,
            0xad,0xe1,0xd6,0x29,0x10,0xf0,0xf9,0x8d,0x07,0xd7,0x0b,0x80,
            0xff,0x81,0xff,0x00,0x04,0x3e,0x04,0x02,0xe8,0x8d,0x00,0xe8,
            0x8d,0x00,0x00,0x00,0x00,0x04,0x06,0x00,0x08,0xe8,0x8d,0x00,
            0xe8,0x8d,0x00,0x00,0x00,0x00,0x00,0x06,0x00,0xe9,0x36,0x8d,
            0x06,0xe8,0x8d,0x00,0x00,0x00,0xa9,0x68,0x5b,0xa5,0x38,0x85,
            0x38,0xa5,0x39,0x85,0x39,0xa5,0x3a,0x85,0x36,0xa5,0x3b,0x85,
            0x37,0xa9,0x3e,0x8d,0xe2,0xd6,0xa9,0x04,0x8d,0xe3,0xd6,0xa2,
            0x0f,0xb5,0x18,0x95,0x26,0xb5,0x28,0x95,0x16,0xca,0x10,0xf5,
            0xa9,0x01,0x8d,0xe4,0xd6,0xa9,0x00,0x5b,0x60
        };

        uint8_t ping_buf[ETHLOAD_PACKET_SIZE] = {};
        std::memcpy(ping_buf, ethlet_echo, sizeof(ethlet_echo));

        auto start = std::chrono::steady_clock::now();
        auto deadline = start + std::chrono::milliseconds(timeout_ms);
        int send_count = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            /* Send echo to unicast address */
            ssize_t s;
            do {
                s = sendto(sockfd_, ping_buf, ETHLOAD_PACKET_SIZE, 0,
                           reinterpret_cast<const sockaddr*>(&dest_),
                           sizeof(dest_));
            } while (s < 0 && errno == EAGAIN);
            send_count++;

            /* Check for response (non-blocking) */
            for (int i = 0; i < 50; i++) {
                uint8_t resp[2048];
                sockaddr_in6 src{};
                socklen_t src_len = sizeof(src);
                ssize_t n = recvfrom(sockfd_, resp, sizeof(resp), 0,
                                     reinterpret_cast<sockaddr*>(&src),
                                     &src_len);
                if (n <= 0) break;

                /* Check if this is the echo response from the MEGA65 */
                if (n == ETHLOAD_PACKET_SIZE &&
                    std::memcmp(&src.sin6_addr, &dest_.sin6_addr, 16) == 0) {
                    if (verbose) {
                        std::println(stderr,
                            "[udp] ETHLOAD responded after {} sends", send_count);
                    }
                    return true;
                }
                /* Skip beacons and other packets */
            }
            usleep(20000);  /* 20ms between retries */
        }

        if (verbose)
            std::println(stderr, "[udp] ETHLOAD did not respond after {} sends",
                         send_count);
        return false;
    }

    std::expected<size_t, TransportError>
    send(std::span<const uint8_t> data) override {
        /* Pad to 1024 bytes — ETHLOAD expects this size */
        uint8_t padded[ETHLOAD_PACKET_SIZE] = {};
        size_t copy_len = std::min(data.size(), ETHLOAD_PACKET_SIZE);
        std::memcpy(padded, data.data(), copy_len);

        ssize_t sent;
        do {
            sent = sendto(sockfd_, padded, ETHLOAD_PACKET_SIZE, 0,
                          reinterpret_cast<const sockaddr*>(&dest_),
                          sizeof(dest_));
        } while (sent < 0 && errno == EAGAIN);

        if (sent < 0) {
            std::println(stderr, "etherdbg: sendto: {}", strerror(errno));
            return std::unexpected(TransportError::SendFailed);
        }
        if (verbose) {
            char addr_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &dest_.sin6_addr, addr_str, sizeof(addr_str));
            std::println(stderr, "[udp] -> sent {} bytes to {}%{}",
                         sent, addr_str, dest_.sin6_scope_id);
        }
        if (trace) {
            for (size_t i = 0; i < data.size(); i++) {
                if (i % 16 == 0) std::print(stderr, "[udp] -> {:04X}: ", i);
                std::print(stderr, "{:02X} ", data[i]);
                if (i % 16 == 15) std::println(stderr, "");
            }
            if (data.size() % 16 != 0) std::println(stderr, "");
        }
        return data.size();  /* return original size, not padded */
    }

    std::expected<std::vector<uint8_t>, TransportError>
    recv(size_t max_len, int timeout_ms) override {
        if (timeout_ms >= 0) {
            fd_set fds;
            timeval tv{};
            FD_ZERO(&fds);
            FD_SET(sockfd_, &fds);
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            int ret = select(sockfd_ + 1, &fds, nullptr, nullptr, &tv);
            if (ret == 0)
                return std::unexpected(TransportError::Timeout);
            if (ret < 0) {
                std::println(stderr, "etherdbg: select: {}", strerror(errno));
                return std::unexpected(TransportError::RecvFailed);
            }
        }

        std::vector<uint8_t> buf(max_len);
        sockaddr_in6 sender{};
        socklen_t sender_len = sizeof(sender);
        ssize_t n = recvfrom(sockfd_, buf.data(), buf.size(), 0,
                             reinterpret_cast<sockaddr*>(&sender),
                             &sender_len);
        if (n < 0) {
            std::println(stderr, "etherdbg: recvfrom: {}", strerror(errno));
            return std::unexpected(TransportError::RecvFailed);
        }
        buf.resize(static_cast<size_t>(n));

        if (verbose) {
            char addr_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &sender.sin6_addr, addr_str, sizeof(addr_str));
            std::println(stderr, "[udp] <- received {} bytes from {}", n, addr_str);
        }
        if (trace) {
            for (size_t i = 0; i < buf.size(); i++) {
                if (i % 16 == 0) std::print(stderr, "[udp] <- {:04X}: ", i);
                std::print(stderr, "{:02X} ", buf[i]);
                if (i % 16 == 15) std::println(stderr, "");
            }
            if (buf.size() % 16 != 0) std::println(stderr, "");
        }
        return buf;
    }

private:
    int sockfd_;
    sockaddr_in6 dest_;
    sockaddr_in6 broadcast_;
};

// ---------------------------------------------------------------------------
// Helper: platform init
// ---------------------------------------------------------------------------

static void platform_init()
{
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
#endif
}

// ---------------------------------------------------------------------------
// create_udp_transport - connect to a known IPv6 address
// ---------------------------------------------------------------------------

std::unique_ptr<Transport> create_udp_transport(std::string_view ip_addr,
                                                 int port)
{
    platform_init();

    int sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::println(stderr, "etherdbg: socket: {}", strerror(errno));
        return nullptr;
    }

    /* Parse "addr%iface" format */
    std::string addr_str(ip_addr);
    std::string iface_name;
    auto pct = addr_str.find('%');
    if (pct != std::string::npos) {
        iface_name = addr_str.substr(pct + 1);
        addr_str = addr_str.substr(0, pct);
    }

    sockaddr_in6 dest{};
    dest.sin6_family = AF_INET6;
    dest.sin6_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET6, addr_str.c_str(), &dest.sin6_addr) != 1) {
        std::println(stderr, "etherdbg: invalid IPv6 address '{}'", ip_addr);
        ::close(sockfd);
        return nullptr;
    }

    if (!iface_name.empty()) {
        unsigned idx = if_nametoindex(iface_name.c_str());
        if (idx == 0) {
            std::println(stderr, "etherdbg: unknown interface '{}'", iface_name);
            ::close(sockfd);
            return nullptr;
        }
        dest.sin6_scope_id = idx;
    }

    /* Set multicast interface so packets go out the correct NIC */
    setsockopt(sockfd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
               &dest.sin6_scope_id, sizeof(dest.sin6_scope_id));

    /* Make socket non-blocking (matching mega65-tools etherload) */
    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL) | O_NONBLOCK);

    /* Set up broadcast address for hyperrupt trigger */
    sockaddr_in6 bcast{};
    bcast.sin6_family = AF_INET6;
    bcast.sin6_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET6, "ff02::1", &bcast.sin6_addr);
    bcast.sin6_scope_id = dest.sin6_scope_id;

    return std::make_unique<UdpTransport>(sockfd, dest, bcast);
}

// ---------------------------------------------------------------------------
// discover_mega65 - listen for "mega65" beacon
// ---------------------------------------------------------------------------

#ifndef _WIN32

struct IfAddr {
    sockaddr_in6 addr;
    unsigned scope_id;
};

static std::vector<IfAddr> enumerate_ipv6_interfaces()
{
    std::vector<IfAddr> result;
    ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0)
        return result;

    for (auto* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;

        auto* sin6 = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
        /* Only link-local addresses (fe80::/10) */
        if (!IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
            continue;

        IfAddr ia{};
        ia.addr = *sin6;
        ia.scope_id = if_nametoindex(ifa->ifa_name);
        result.push_back(ia);
    }
    freeifaddrs(ifap);
    return result;
}

#endif // !_WIN32

std::string discover_mega65(int timeout_ms, bool verbose_flag)
{
    platform_init();

#ifdef _WIN32
    /* TODO: Windows interface enumeration */
    std::println(stderr, "etherdbg: auto-discovery not yet supported on Windows");
    return "";
#else
    auto interfaces = enumerate_ipv6_interfaces();
    if (interfaces.empty()) {
        std::println(stderr,
            "etherdbg: no IPv6 link-local interfaces found.\n"
            "          Ensure IPv6 is enabled on the interface connected to the MEGA65.");
        return "";
    }

    if (verbose_flag)
        std::println("Listening on {} interface(s) for MEGA65 beacon...",
                     interfaces.size());

    /* Create a single listening socket on port 4510.
     * No IPV6_JOIN_GROUP — joining ff02::1 causes the OS to send MLD
     * reports which ETHLOAD misinterprets, corrupting the MEGA65 screen.
     * We receive beacons anyway since ff02::1 is the all-nodes group
     * that all IPv6 hosts implicitly listen on. */
    int discoverfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (discoverfd < 0) {
        std::println(stderr, "etherdbg: failed to create discovery socket");
        return "";
    }
    fcntl(discoverfd, F_SETFL, fcntl(discoverfd, F_GETFL) | O_NONBLOCK);
    int enable = 1;
    setsockopt(discoverfd, IPPROTO_IPV6, IPV6_V6ONLY, &enable, sizeof(enable));
    setsockopt(discoverfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    setsockopt(discoverfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));

    sockaddr_in6 bind_addr{};
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_port = htons(DEFAULT_PORT);
    bind_addr.sin6_addr = in6addr_any;
    if (bind(discoverfd, reinterpret_cast<sockaddr*>(&bind_addr),
             sizeof(bind_addr)) < 0) {
        std::println(stderr, "etherdbg: failed to bind discovery socket");
        ::close(discoverfd);
        return "";
    }

    /* Poll for "mega65" beacon */
    std::string result;
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        char buf[256];
        sockaddr_in6 src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(discoverfd, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&src), &src_len);
        if (n == 6 && std::memcmp(buf, "mega65", 6) == 0) {
            char addr_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &src.sin6_addr, addr_str, sizeof(addr_str));

            /* Build "addr%iface" string */
            char iface_name[IF_NAMESIZE] = {};
            if_indextoname(src.sin6_scope_id, iface_name);

            result = std::string(addr_str) + "%" + iface_name;
            break;
        }
        usleep(1000);
    }

    ::close(discoverfd);

    if (result.empty()) {
        std::println(stderr,
            "etherdbg: no MEGA65 found on the local network.\n"
            "          Ensure remote control is active (Shift+Pound, LED blinking).");
    }
    else if (verbose_flag) {
        std::println("Discovered MEGA65 at {}", result);
    }

    return result;
#endif // _WIN32
}

// ---------------------------------------------------------------------------
// create_udp_transport_auto - discover then connect
// ---------------------------------------------------------------------------

std::unique_ptr<Transport> create_udp_transport_auto(int port, bool verbose_flag)
{
    auto addr = discover_mega65(DEFAULT_DISCOVER_TIMEOUT_MS, verbose_flag);
    if (addr.empty())
        return nullptr;

    return create_udp_transport(addr, port);
}

} // namespace etherdbg
