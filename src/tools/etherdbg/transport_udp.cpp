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

namespace etherdbg {

// ---------------------------------------------------------------------------
// UdpTransport - IPv6 UDP implementation
// ---------------------------------------------------------------------------

class UdpTransport final : public Transport {
public:
    UdpTransport(int sockfd, sockaddr_in6 dest)
        : sockfd_(sockfd), dest_(dest) {}

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

    std::expected<size_t, TransportError>
    send(std::span<const uint8_t> data) override {
        ssize_t sent = sendto(sockfd_, data.data(), data.size(), 0,
                              reinterpret_cast<const sockaddr*>(&dest_),
                              sizeof(dest_));
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
        return static_cast<size_t>(sent);
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
        return buf;
    }

private:
    int sockfd_;
    sockaddr_in6 dest_;
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

    /* Set multicast interface for link-local */
    setsockopt(sockfd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
               &dest.sin6_scope_id, sizeof(dest.sin6_scope_id));

    /* Bind to port 4510 so we can receive replies.
     * The MEGA65's read routine swaps src/dst ports, so the response
     * comes back to whatever port we sent from. We must send FROM
     * port 4510 (which ETHLOAD expects) and receive ON port 4510. */
    int enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#ifndef _WIN32
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
#endif

    sockaddr_in6 bind_addr{};
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_port = htons(static_cast<uint16_t>(port));
    bind_addr.sin6_addr = in6addr_any;
    if (bind(sockfd, reinterpret_cast<sockaddr*>(&bind_addr),
             sizeof(bind_addr)) < 0) {
        std::println(stderr, "etherdbg: bind port {}: {}", port, strerror(errno));
        ::close(sockfd);
        return nullptr;
    }

    return std::make_unique<UdpTransport>(sockfd, dest);
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

    /* Create a listening socket per interface */
    std::vector<int> fds;
    for (auto& iface : interfaces) {
        int fd = socket(AF_INET6, SOCK_DGRAM, 0);
        if (fd < 0) continue;

        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

        int enable = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enable, sizeof(enable));
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));

        /* Join multicast group ff02::1 on this interface */
        ipv6_mreq mreq{};
        mreq.ipv6mr_interface = iface.scope_id;
        inet_pton(AF_INET6, "ff02::1", &mreq.ipv6mr_multiaddr);
        setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq));

        sockaddr_in6 bind_addr{};
        bind_addr.sin6_family = AF_INET6;
        bind_addr.sin6_port = htons(DEFAULT_PORT);
        bind_addr.sin6_addr = in6addr_any;
        if (bind(fd, reinterpret_cast<sockaddr*>(&bind_addr),
                 sizeof(bind_addr)) < 0) {
            ::close(fd);
            continue;
        }
        fds.push_back(fd);
    }

    if (fds.empty()) {
        std::println(stderr, "etherdbg: failed to create discovery sockets");
        return "";
    }

    /* Poll for "mega65" beacon */
    std::string result;
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        for (int fd : fds) {
            char buf[256];
            sockaddr_in6 src{};
            socklen_t src_len = sizeof(src);
            ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&src), &src_len);
            if (n == 6 && std::memcmp(buf, "mega65", 6) == 0) {
                char addr_str[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &src.sin6_addr, addr_str, sizeof(addr_str));

                /* Build "addr%iface" string */
                char iface_name[IF_NAMESIZE] = {};
                if_indextoname(src.sin6_scope_id, iface_name);

                result = std::string(addr_str) + "%" + iface_name;
                goto done;
            }
        }
        usleep(1000);
    }

done:
    for (int fd : fds)
        ::close(fd);

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
