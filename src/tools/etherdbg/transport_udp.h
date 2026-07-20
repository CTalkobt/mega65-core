/*
 * transport_udp.h - UDP transport for MEGA65 Ethernet debug protocol
 *
 * Uses IPv6 link-local addressing to match ETHLOAD.M65's protocol.
 * Auto-discovery listens for the MEGA65's "mega65" beacon on port 4510.
 */

#pragma once

#include "transport.h"
#include <memory>
#include <string>
#include <string_view>

namespace etherdbg {

inline constexpr int DEFAULT_PORT = 4510;
inline constexpr int DEFAULT_DISCOVER_TIMEOUT_MS = 3000;

/*
 * Create a UDP transport connected to the given IPv6 address.
 * Address should include scope ID (e.g. "fe80::1234%eth0").
 * Returns nullptr on failure.
 */
std::unique_ptr<Transport> create_udp_transport(std::string_view ip_addr,
                                                 int port = DEFAULT_PORT);

/*
 * Auto-discover a MEGA65 on the local network.
 * Listens for the "mega65" beacon on all IPv6-capable interfaces.
 * Returns the discovered IPv6 address (with scope ID) or empty string.
 */
std::string discover_mega65(int timeout_ms = DEFAULT_DISCOVER_TIMEOUT_MS,
                             bool verbose = false);

/*
 * Create a UDP transport by auto-discovering the MEGA65.
 * Combines discover_mega65() + create_udp_transport().
 * Returns nullptr if no MEGA65 found.
 */
std::unique_ptr<Transport> create_udp_transport_auto(
    int port = DEFAULT_PORT, bool verbose = false);

} // namespace etherdbg
