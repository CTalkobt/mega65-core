/*
 * transport_udp.h - UDP transport for MEGA65 Ethernet debug protocol
 */

#pragma once

#include "transport.h"
#include <memory>
#include <string_view>

namespace etherdbg {

inline constexpr int DEFAULT_PORT = 4510;

/*
 * Create a UDP transport connected to the given IP address and port.
 * Returns nullptr on failure.
 */
std::unique_ptr<Transport> create_udp_transport(std::string_view ip_addr,
                                                 int port = DEFAULT_PORT);

/*
 * Create a UDP transport that broadcasts to discover the MEGA65.
 * Sends to 255.255.255.255. When a response is received, the sender's
 * IP is captured and used for all subsequent sends.
 * Returns nullptr on failure.
 */
std::unique_ptr<Transport> create_udp_transport_broadcast(int port = DEFAULT_PORT);

} // namespace etherdbg
