/*
 * transport_udp.c - UDP transport implementation for MEGA65 Ethernet debug
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "transport.h"
#include "transport_udp.h"

struct transport_udp {
    struct transport base;
    int sockfd;
    struct sockaddr_in dest;
};

static int udp_send(struct transport *t, const void *data, size_t len)
{
    struct transport_udp *u = (struct transport_udp *)t;
    ssize_t sent = sendto(u->sockfd, data, len, 0,
                          (struct sockaddr *)&u->dest, sizeof(u->dest));
    if (sent < 0) {
        perror("etherdbg: sendto");
        return TRANSPORT_ERR_SEND;
    }
    if (t->verbose)
        fprintf(stderr, "[udp] sent %zd bytes\n", sent);
    return TRANSPORT_OK;
}

static int udp_recv(struct transport *t, void *buf, size_t buflen, int timeout_ms)
{
    struct transport_udp *u = (struct transport_udp *)t;

    if (timeout_ms >= 0) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(u->sockfd, &fds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int ret = select(u->sockfd + 1, &fds, NULL, NULL, &tv);
        if (ret == 0)
            return TRANSPORT_ERR_TIMEOUT;
        if (ret < 0) {
            perror("etherdbg: select");
            return TRANSPORT_ERR_RECV;
        }
    }

    ssize_t n = recvfrom(u->sockfd, buf, buflen, 0, NULL, NULL);
    if (n < 0) {
        perror("etherdbg: recvfrom");
        return TRANSPORT_ERR_RECV;
    }
    if (t->verbose)
        fprintf(stderr, "[udp] received %zd bytes\n", n);
    return (int)n;
}

static void udp_close(struct transport *t)
{
    struct transport_udp *u = (struct transport_udp *)t;
#ifdef _WIN32
    closesocket(u->sockfd);
    WSACleanup();
#else
    close(u->sockfd);
#endif
    free(u);
}

static const struct transport_ops udp_ops = {
    .send  = udp_send,
    .recv  = udp_recv,
    .close = udp_close,
};

struct transport *transport_udp_create(const char *ip_addr, int port)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "etherdbg: WSAStartup failed\n");
        return NULL;
    }
#endif

    struct transport_udp *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;

    u->base.ops = &udp_ops;
    u->base.verbose = 0;

    u->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (u->sockfd < 0) {
        perror("etherdbg: socket");
        free(u);
        return NULL;
    }

    int broadcastEnable = 1;
    setsockopt(u->sockfd, SOL_SOCKET, SO_BROADCAST,
               (char *)&broadcastEnable, sizeof(broadcastEnable));

    memset(&u->dest, 0, sizeof(u->dest));
    u->dest.sin_family = AF_INET;
    u->dest.sin_addr.s_addr = inet_addr(ip_addr);
    u->dest.sin_port = htons(port);

    if (u->dest.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "etherdbg: invalid IP address '%s'\n", ip_addr);
        close(u->sockfd);
        free(u);
        return NULL;
    }

    return &u->base;
}
