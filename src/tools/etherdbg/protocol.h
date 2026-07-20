/*
 * protocol.h - MEGA65 debug protocol: packet construction and constants
 *
 * This layer builds the executable payloads that the MEGA65's ETHLOAD.M65
 * listener will JSR into. It is transport-agnostic -- it constructs byte
 * buffers that the transport layer delivers.
 *
 * Protocol overview:
 *   - Each packet payload must begin with $A9 (LDA #imm) for the MEGA65
 *     listener to recognise it as executable.
 *   - "DMA load" packets contain a small 45GS02 routine + embedded DMA list
 *     that copies up to 1024 bytes of data to a target address.
 *   - The "all done" packet restores normal memory mapping and returns
 *     control to the MEGA65.
 */

#ifndef ETHERDBG_PROTOCOL_H
#define ETHERDBG_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Maximum data payload per DMA load packet */
#define PROTO_MAX_CHUNK_SIZE 1024

/* Total packet buffer size (routine + DMA list + data) */
#define PROTO_DMA_PACKET_SIZE (128 + PROTO_MAX_CHUNK_SIZE)

/* All-done packet size */
#define PROTO_DONE_PACKET_SIZE 128

/*
 * Build a DMA load packet that will copy 'data_len' bytes to the given
 * 28-bit MEGA65 address when executed on the target.
 *
 * buf:       output buffer, must be at least PROTO_DMA_PACKET_SIZE bytes
 * addr:      destination address (bottom 16 bits)
 * bank:      destination bank byte
 * mb:        destination megabyte
 * data:      payload bytes to copy (max PROTO_MAX_CHUNK_SIZE)
 * data_len:  number of payload bytes
 * seq:       sequence number for this packet
 *
 * Returns the total packet size to send.
 */
int proto_build_dma_load(uint8_t *buf, uint16_t addr, uint8_t bank,
                         uint8_t mb, const uint8_t *data, int data_len,
                         uint8_t seq);

/*
 * Build the "all done" packet that restores normal memory mapping
 * and returns control to the MEGA65.
 *
 * buf:  output buffer, must be at least PROTO_DONE_PACKET_SIZE bytes
 *
 * Returns the total packet size to send.
 */
int proto_build_done(uint8_t *buf);

#endif /* ETHERDBG_PROTOCOL_H */
