/*
 * transport_udp.h - UDP transport for MEGA65 Ethernet debug protocol
 */

#ifndef ETHERDBG_TRANSPORT_UDP_H
#define ETHERDBG_TRANSPORT_UDP_H

#include "transport.h"

#define ETHERDBG_DEFAULT_PORT 4510

/*
 * Create a UDP transport connected to the given IP address and port.
 * Returns NULL on failure.
 */
struct transport *transport_udp_create(const char *ip_addr, int port);

#endif /* ETHERDBG_TRANSPORT_UDP_H */
