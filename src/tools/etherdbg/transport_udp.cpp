/*
 * transport_udp.cpp - UDP transport implementation for MEGA65 Ethernet debug
 *
 * Sends UDP packets to the MEGA65's Ethernet listener (port 4510).
 * The MEGA65 runs ETHLOAD.M65 (activated by Shift+pound) which receives
 * and executes code embedded in UDP payloads.
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
    UdpTransport(int sockfd, sockaddr_in dest)
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
        if (verbose)
            std::println(stderr, "[udp] sent {} bytes", sent);
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
        ssize_t n = recvfrom(sockfd_, buf.data(), buf.size(), 0,
                             nullptr, nullptr);
        if (n < 0) {
            std::println(stderr, "etherdbg: recvfrom: {}", strerror(errno));
            return std::unexpected(TransportError::RecvFailed);
        }
        buf.resize(static_cast<size_t>(n));
        if (verbose)
            std::println(stderr, "[udp] received {} bytes", n);
        return buf;
    }

private:
    int sockfd_;
    sockaddr_in dest_;
};

std::unique_ptr<Transport> create_udp_transport(std::string_view ip_addr,
                                                 int port)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::println(stderr, "etherdbg: WSAStartup failed");
        return nullptr;
    }
#endif

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::println(stderr, "etherdbg: socket: {}", strerror(errno));
        return nullptr;
    }

    int broadcastEnable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<char*>(&broadcastEnable),
               sizeof(broadcastEnable));

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

    auto transport = std::make_unique<UdpTransport>(sockfd, dest);
    return transport;
}

} // namespace etherdbg
