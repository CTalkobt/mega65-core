/*
 * protocol.c - MEGA65 debug protocol: packet construction
 *
 * Builds executable 45GS02 routines that the MEGA65's ETHLOAD.M65
 * listener will JSR into when received via UDP port 4510.
 *
 * The DMA load routine:
 *   - Sets DMA source MB to $FF (the Ethernet buffer lives at $FFDE800)
 *   - Sets DMA destination MB from the packet
 *   - Points the DMA list address to the embedded list
 *   - Triggers DMA to copy payload data to the target address
 *   - Tracks sequence number for debugging
 *
 * The all-done routine:
 *   - Copies a small finalisation stub to the cassette buffer ($0340)
 *   - Jumps there to safely unmap the Ethernet buffer
 *   - Restores normal MAP state and returns via RTS
 */

#include <string.h>
#include "protocol.h"

/*
 * Offsets within the DMA load packet for patchable fields.
 * These correspond to the positions in the embedded DMA list.
 */
#define DMA_BYTE_COUNT_OFFSET       0x31
#define DMA_DEST_ADDR_OFFSET        0x36
#define DMA_DEST_BANK_OFFSET        0x38
#define DMA_PACKET_NUMBER_OFFSET    0x3b
#define DMA_DEST_MB_OFFSET          0x3c
#define DMA_DATA_OFFSET             (0x80 - 0x2c)

/* Base DMA load routine -- 45GS02 machine code + embedded DMA list */
static const uint8_t dma_load_template[] = {
    /* $00: 45GS02 routine to set up and trigger DMA */
    0xa9, 0xff,             /* LDA #$FF           ; source MB = $FF          */
    0x8d, 0x05, 0xd7,       /* STA $D705          ; set DMA source MB        */
    0xad, 0x68, 0x68,       /* LDA $6868 (dest_mb); load dest MB from packet */
    0x8d, 0x06, 0xd7,       /* STA $D706          ; set DMA dest MB          */
    0xa9, 0x0d,             /* LDA #$0D           ; DMA list bank high       */
    0x8d, 0x02, 0xd7,       /* STA $D702                                     */
    0xa9, 0xe8,             /* LDA #$E8           ; DMA list address high    */
    0x8d, 0x01, 0xd7,       /* STA $D701                                     */
    0xa9, 0xff,             /* LDA #$FF           ; DMA list MB              */
    0x8d, 0x04, 0xd7,       /* STA $D704                                     */
    0xa9, 0x5c,             /* LDA #$5C           ; DMA list address low     */
    0x8d, 0x00, 0xd7,       /* STA $D700          ; trigger DMA              */

    /* $1e: Debug: store packet seq number at $0680+X */
    0xae, 0x67, 0x68,       /* LDX $6867 (pkt#)                              */
    0xea,                   /* NOP                                           */
    0x9d, 0x80, 0x06,       /* STA $0680,X                                   */

    /* $25: Increment 16-bit counter at $0425-$0426 */
    0xee, 0x26, 0x04,       /* INC $0426                                     */
    0xd0, 0x03,             /* BNE +3                                        */
    0xee, 0x25, 0x04,       /* INC $0425                                     */

    /* $2e: Return to listener loop */
    0x60,                   /* RTS                                           */

    /* $2f: Padding to $30 */
    0x00,

    /* $30: Embedded F018B DMA list */
    0x00,                   /* DMA command: copy, no chain                   */
    0x00, 0x04,             /* Byte count (patched per packet)               */
    0x80, 0xe8, 0x8d,       /* Source: $FFDE880 (data area in ETH RX buffer) */
    0x00, 0x10,             /* Dest address low 16 (patched per packet)      */
    0x00,                   /* Dest bank (patched per packet)                */
    0x00, 0x00,             /* Modulo (unused)                               */
    0x30,                   /* Packet sequence number (patched per packet)   */
    0x00,                   /* Dest MB (patched per packet)                  */
    0x00, 0x00, 0x00,       /* Padding                                       */
};

/* All-done routine -- restores memory mapping and exits listener */
static const uint8_t done_template[] = {
    /* $00: Border flash for visual feedback */
    0xa9, 0x00,             /* LDA #$00                                      */
    0xee, 0x20, 0xd0,       /* INC $D020                                     */
    0x4c, 0x2c, 0x68,       /* JMP $682C (skip to copy-to-cassette-buffer)   */

    /* $08: Second LDA #$00 + 6 NOPs (reserved/padding) */
    0xa9, 0x00,             /* LDA #$00                                      */
    0xea, 0xea, 0xea,       /* NOP NOP NOP                                   */
    0xea, 0xea, 0xea,       /* NOP NOP NOP                                   */

    /* $10: Copy finalisation stub ($6844+) to cassette buffer ($0340) */
    0xa2, 0x00,             /* LDX #$00                                      */
    /* loop: */
    0xbd, 0x44, 0x68,       /* LDA $6844,X  (offset into this packet)       */
    0x9d, 0x40, 0x03,       /* STA $0340,X                                   */
    0xe8,                   /* INX                                           */
    0xe0, 0x40,             /* CPX #$40                                      */
    0xd0, 0xf5,             /* BNE loop                                      */
    0x4c, 0x40, 0x03,       /* JMP $0340    (execute from cassette buffer)   */

    /* $1e: Finalisation stub (copied to $0340, then executed from there) */
    /* Enable MEGA65 I/O */
    0xa9, 0x47,             /* LDA #$47                                      */
    0x8d, 0x2f, 0xd0,       /* STA $D02F                                     */
    0xa9, 0x53,             /* LDA #$53                                      */
    0x8d, 0x2f, 0xd0,       /* STA $D02F                                     */

    /* Restore default MAP: clear all mapping */
    0xa9, 0x00,             /* LDA #$00                                      */
    0xa2, 0x0f,             /* LDX #$0F                                      */
    0xa0, 0x00,             /* LDY #$00                                      */
    0xa3, 0x00,             /* LDZ #$00                                      */
    0x5c,                   /* MAP                                           */
    0xea,                   /* EOM (NOP)                                     */

    /* Second MAP to fully clear */
    0xa9, 0x00,             /* LDA #$00                                      */
    0xa2, 0x00,             /* LDX #$00                                      */
    0xa0, 0x00,             /* LDY #$00                                      */
    0xa3, 0x00,             /* LDZ #$00                                      */
    0x5c,                   /* MAP                                           */
    0xea,                   /* EOM (NOP)                                     */

    /* Pop JSR return address and return to caller */
    0x68,                   /* PLA                                           */
    0x68,                   /* PLA                                           */
    0x60,                   /* RTS                                           */
};

int proto_build_dma_load(uint8_t *buf, uint16_t addr, uint8_t bank,
                         uint8_t mb, const uint8_t *data, int data_len,
                         uint8_t seq)
{
    /* Start from template */
    memset(buf, 0, PROTO_DMA_PACKET_SIZE);
    memcpy(buf, dma_load_template, sizeof(dma_load_template));

    /* Patch destination address */
    buf[DMA_DEST_ADDR_OFFSET]     = addr & 0xff;
    buf[DMA_DEST_ADDR_OFFSET + 1] = (addr >> 8) & 0xff;
    buf[DMA_DEST_BANK_OFFSET]     = bank;
    buf[DMA_DEST_MB_OFFSET]       = mb;

    /* Patch byte count */
    buf[DMA_BYTE_COUNT_OFFSET]     = data_len & 0xff;
    buf[DMA_BYTE_COUNT_OFFSET + 1] = (data_len >> 8) & 0xff;

    /* Patch sequence number */
    buf[DMA_PACKET_NUMBER_OFFSET] = seq;

    /* Copy data payload */
    if (data && data_len > 0)
        memcpy(&buf[DMA_DATA_OFFSET], data, data_len);

    return PROTO_DMA_PACKET_SIZE;
}

int proto_build_done(uint8_t *buf)
{
    memset(buf, 0, PROTO_DONE_PACKET_SIZE);
    memcpy(buf, done_template, sizeof(done_template));
    return PROTO_DONE_PACKET_SIZE;
}
