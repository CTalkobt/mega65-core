/*
 * transport_udp.cpp - UDP transport implementation for MEGA65 Ethernet debug
 *
 * Sends UDP packets to the MEGA65's Ethernet listener (port 4510).
 * The MEGA65 runs ETHLOAD.M65 (activated by Shift+pound) which receives
 * and executes code embedded in UDP payloads.
 *
 * Supports two modes:
 *   - Direct: send to a specific IP address
 *   - Broadcast: send to 255.255.255.255, learn the MEGA65's IP from the
 *     first response, then direct all subsequent traffic there
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include <cstring>
#include <print>
#include <string>

#include "transport_udp.h"

namespace etherdbg {

class UdpTransport final : public Transport {
public:
    UdpTransport(int sockfd, sockaddr_in dest, bool broadcast_mode)
        : sockfd_(sockfd), dest_(dest), broadcast_mode_(broadcast_mode) {}

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
        if (verbose)
            std::println(stderr, "[udp] sent {} bytes to {}",
                         sent, inet_ntoa(dest_.sin_addr));
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
        sockaddr_in sender{};
        socklen_t sender_len = sizeof(sender);
        ssize_t n = recvfrom(sockfd_, buf.data(), buf.size(), 0,
                             reinterpret_cast<sockaddr*>(&sender),
                             &sender_len);
        if (n < 0) {
            std::println(stderr, "etherdbg: recvfrom: {}", strerror(errno));
            return std::unexpected(TransportError::RecvFailed);
        }
        buf.resize(static_cast<size_t>(n));

        /* If in broadcast mode and we haven't learned the peer yet,
         * capture the sender's address for all future sends. */
        if (broadcast_mode_ && !peer_learned_) {
            dest_.sin_addr = sender.sin_addr;
            peer_learned_ = true;
            std::println(stderr, "etherdbg: discovered MEGA65 at {}",
                         inet_ntoa(sender.sin_addr));
        }

        if (verbose)
            std::println(stderr, "[udp] received {} bytes from {}",
                         n, inet_ntoa(sender.sin_addr));
        return buf;
    }

private:
    int sockfd_;
    sockaddr_in dest_;
    bool broadcast_mode_ = false;
    bool peer_learned_ = false;
};

static int create_socket_common()
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::println(stderr, "etherdbg: WSAStartup failed");
        return -1;
    }
#endif

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::println(stderr, "etherdbg: socket: {}", strerror(errno));
        return -1;
    }

    int broadcastEnable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<char*>(&broadcastEnable),
               sizeof(broadcastEnable));

    return sockfd;
}

std::unique_ptr<Transport> create_udp_transport(std::string_view ip_addr,
                                                 int port)
{
    int sockfd = create_socket_common();
    if (sockfd < 0)
        return nullptr;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    std::string ip_str(ip_addr);
    dest.sin_addr.s_addr = inet_addr(ip_str.c_str());
    dest.sin_port = htons(static_cast<uint16_t>(port));

    if (dest.sin_addr.s_addr == INADDR_NONE) {
        std::println(stderr, "etherdbg: invalid IP address '{}'", ip_addr);
        ::close(sockfd);
        return nullptr;
    }

    return std::make_unique<UdpTransport>(sockfd, dest, false);
}

std::unique_ptr<Transport> create_udp_transport_broadcast(int port)
{
    int sockfd = create_socket_common();
    if (sockfd < 0)
        return nullptr;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = INADDR_BROADCAST;
    dest.sin_port = htons(static_cast<uint16_t>(port));

    return std::make_unique<UdpTransport>(sockfd, dest, true);
}

} // namespace etherdbg
