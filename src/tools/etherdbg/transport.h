/*
 * transport.h - Abstract transport interface for MEGA65 debug communication
 *
 * Provides a transport-agnostic interface for sending and receiving packets.
 * Implementations: UDP/Ethernet (transport_udp.c), future: JTAG serial.
 */

#ifndef ETHERDBG_TRANSPORT_H
#define ETHERDBG_TRANSPORT_H

#include <stddef.h>

/* Error codes */
#define TRANSPORT_OK        0
#define TRANSPORT_ERR_INIT -1
#define TRANSPORT_ERR_SEND -2
#define TRANSPORT_ERR_RECV -3
#define TRANSPORT_ERR_TIMEOUT -4

/* Forward declaration */
struct transport;

/*
 * Transport operations vtable.
 * Each transport backend (UDP, JTAG, etc.) provides an implementation.
 */
struct transport_ops {
    /* Send a packet. Returns TRANSPORT_OK or error code. */
    int (*send)(struct transport *t, const void *data, size_t len);

    /* Receive a packet. Returns bytes received, 0 on timeout, or negative error.
     * timeout_ms: milliseconds to wait (0 = non-blocking, -1 = indefinite) */
    int (*recv)(struct transport *t, void *buf, size_t buflen, int timeout_ms);

    /* Close and free resources */
    void (*close)(struct transport *t);
};

/*
 * Base transport handle. Transport backends embed this as their first member.
 */
struct transport {
    const struct transport_ops *ops;
    int verbose;
};

/* Convenience wrappers */
static inline int transport_send(struct transport *t, const void *data, size_t len)
{
    return t->ops->send(t, data, len);
}

static inline int transport_recv(struct transport *t, void *buf, size_t buflen, int timeout_ms)
{
    return t->ops->recv(t, buf, buflen, timeout_ms);
}

static inline void transport_close(struct transport *t)
{
    t->ops->close(t);
}

#endif /* ETHERDBG_TRANSPORT_H */
