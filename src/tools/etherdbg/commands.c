/*
 * commands.c - High-level MEGA65 debug commands
 */

#define _DEFAULT_SOURCE  /* for usleep */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#include "commands.h"
#include "protocol.h"
#include "transport.h"

int cmd_load_program(struct transport *t, const char *filename, int verbose)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror(filename);
        return -1;
    }

    /* Read 2-byte PRG load address */
    uint8_t hdr[2];
    if (read(fd, hdr, 2) < 2) {
        fprintf(stderr, "etherdbg: failed to read load address from '%s'\n",
                filename);
        close(fd);
        return -1;
    }

    uint32_t address = hdr[0] | (hdr[1] << 8);
    if (verbose)
        printf("Load address: $%04X\n", address);

    uint8_t packet[PROTO_DMA_PACKET_SIZE];
    uint8_t filebuf[PROTO_MAX_CHUNK_SIZE];
    int total_bytes = 0;
    uint8_t seq = 0;
    int bytes;

    while ((bytes = read(fd, filebuf, PROTO_MAX_CHUNK_SIZE)) > 0) {
        if (verbose)
            printf("  Sending %d bytes -> $%04X\n", bytes, address);

        uint16_t addr16 = address & 0xffff;
        uint8_t bank = (address >> 16) & 0xff;
        uint8_t mb = (address >> 20) & 0xff;

        int pktlen = proto_build_dma_load(packet, addr16, bank, mb,
                                          filebuf, bytes, seq);
        int ret = transport_send(t, packet, pktlen);
        if (ret != TRANSPORT_OK) {
            fprintf(stderr, "etherdbg: send failed at offset %d\n",
                    total_bytes);
            close(fd);
            return -1;
        }

        usleep(CMD_DEFAULT_PACKET_DELAY_US);
        seq++;
        address += bytes;
        total_bytes += bytes;
    }

    close(fd);

    if (verbose)
        printf("Sent %d bytes from '%s'\n", total_bytes, filename);

    /* Send "all done" packet multiple times for reliability */
    uint8_t done_pkt[PROTO_DONE_PACKET_SIZE];
    int done_len = proto_build_done(done_pkt);

    if (verbose)
        printf("Sending completion signal");

    for (int i = 0; i < CMD_DONE_REPEAT_COUNT; i++) {
        transport_send(t, done_pkt, done_len);
        usleep(CMD_DEFAULT_PACKET_DELAY_US);
        if (verbose) {
            printf(".");
            fflush(stdout);
        }
    }

    if (verbose)
        printf(" done\n");

    return 0;
}
