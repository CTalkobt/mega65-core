/*
 * transport.h - Abstract transport interface for MEGA65 debug communication
 *
 * Provides a transport-agnostic interface for sending and receiving packets.
 * Implementations: UDP/Ethernet (transport_udp.cpp), future: JTAG serial.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace etherdbg {

enum class TransportError {
    InitFailed,
    SendFailed,
    RecvFailed,
    Timeout,
};

inline std::string to_string(TransportError e) {
    switch (e) {
        case TransportError::InitFailed: return "initialization failed";
        case TransportError::SendFailed: return "send failed";
        case TransportError::RecvFailed: return "receive failed";
        case TransportError::Timeout:    return "timeout";
    }
    return "unknown error";
}

/*
 * Abstract transport interface.
 * Each transport backend (UDP, JTAG, etc.) derives from this.
 */
class Transport {
public:
    virtual ~Transport() = default;

    /* Send a packet. Returns number of bytes sent or error. */
    virtual std::expected<size_t, TransportError>
    send(std::span<const uint8_t> data) = 0;

    /* Receive a packet. Returns received bytes or error.
     * timeout_ms: milliseconds to wait (0 = non-blocking, -1 = indefinite) */
    virtual std::expected<std::vector<uint8_t>, TransportError>
    recv(size_t max_len, int timeout_ms) = 0;

    /* Send a transport-specific initialization/trigger sequence.
     * For UDP: sends the hyperrupt trigger to activate ETHLOAD.
     * For JTAG: no-op (monitor is always active). */
    virtual void activate() {}

    bool verbose = false;
    bool trace = false;   /* -vv: hex dump all sent/received packets */
};

} // namespace etherdbg
