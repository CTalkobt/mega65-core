/*
 * protocol.cpp - MEGA65 debug protocol: packet construction
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

#include <algorithm>
#include <cstring>
#include "protocol.h"

namespace etherdbg::protocol {

namespace {

/* Offsets within the DMA load packet for patchable fields */
constexpr int DMA_BYTE_COUNT_OFFSET    = 0x31;
constexpr int DMA_DEST_ADDR_OFFSET     = 0x36;
constexpr int DMA_DEST_BANK_OFFSET     = 0x38;
constexpr int DMA_PACKET_NUMBER_OFFSET = 0x3b;
constexpr int DMA_DEST_MB_OFFSET       = 0x3c;
constexpr int DMA_DATA_OFFSET          = 0x80 - 0x2c;

/* Base DMA load routine -- 45GS02 machine code + embedded DMA list */
constexpr uint8_t dma_load_template[] = {
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
constexpr uint8_t done_template[] = {
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

/*
 * Memory read routine -- 45GS02 code that runs on the MEGA65.
 *
 * When executed, this routine:
 *   1. Copies requester's source MAC → TX buffer dest MAC ($6000)
 *   2. Copies our MAC ($D6E9) → TX buffer source MAC ($6006)
 *   3. Writes an etherdbg response header (EtherType $6502, 'R', seq, addr, count)
 *   4. DMA-copies 'count' bytes from the target address into TX buffer at $6016
 *   5. Sets TX frame size and triggers transmit via $D6E4
 *   6. Returns (RTS) to the ETHLOAD listener loop
 *
 * Patchable fields in the packet:
 *   - Source address for DMA (target memory to read)
 *   - Byte count
 *   - Sequence number and address echo in the response header
 *
 * Memory layout when ETHLOAD is running (basepage restored to $00 before JSR):
 *   $6000-$67FF  TX buffer (mapped from $FFDE000)
 *   $6800-$6FFF  RX buffer (mapped from $FFDE800)
 *   $6808-$680D  Requester's source MAC (in received Ethernet frame)
 *   $D6E9-$D6EE  Our MAC address
 *   $D6E2-$D6E3  TX frame size (little-endian)
 *   $D6E4         TX trigger (write $01)
 */

/* Offsets within the mem_read packet for patchable fields */
constexpr int MR_SEQ_OFFSET    = 0x03;  /* LDA #seq at byte 2-3 */
constexpr int MR_ADDR_OFFSET   = 0x50;  /* response header: source address */
constexpr int MR_COUNT_OFFSET  = 0x54;  /* response header: byte count */
constexpr int MR_TXSIZE_OFFSET = 0x56;  /* TX frame size (total) */

/* DMA list patchable fields within the read packet */
constexpr int MR_DMA_COUNT_OFFSET   = 0x61;  /* DMA byte count */
constexpr int MR_DMA_SRC_ADDR_OFFSET = 0x63; /* DMA source addr low 16 */
constexpr int MR_DMA_SRC_BANK_OFFSET = 0x65; /* DMA source bank */
constexpr int MR_DMA_SRC_MB_OFFSET   = 0x58; /* DMA source MB (in setup code) */

/*
 * The mem_read routine is built as a relocatable sequence starting at $6840
 * (where ETHLOAD JSRs into packet payloads).
 *
 * We construct this programmatically rather than as a static template because
 * the response header bytes and DMA list fields need patching at multiple
 * interdependent offsets.
 */

/* Helper to emit bytes into a vector at a given offset */
static void emit(std::vector<uint8_t>& buf, int& pc, uint8_t byte) {
    if (pc >= static_cast<int>(buf.size()))
        buf.resize(pc + 1, 0);
    buf[pc++] = byte;
}

static void emit16(std::vector<uint8_t>& buf, int& pc, uint16_t val) {
    emit(buf, pc, val & 0xff);
    emit(buf, pc, (val >> 8) & 0xff);
}

static std::vector<uint8_t> build_mem_read_routine(uint32_t address,
                                                    uint16_t count,
                                                    uint8_t seq)
{
    /*
     * Build a proper IPv6 UDP response by copying the incoming packet's
     * headers from RX to TX, swapping src/dst, and appending our payload.
     *
     * RX buffer layout (basepage $00, absolute addresses):
     *   $6802-$6807  Dest MAC (us)
     *   $6808-$680D  Source MAC (requester)
     *   $680E-$680F  EtherType ($86DD)
     *   $6810-$6817  IPv6 ver/class/flow + payload_len + next_hdr + hop
     *   $6818-$6827  IPv6 source addr (requester, 16 bytes)
     *   $6828-$6837  IPv6 dest addr (us, 16 bytes)
     *   $6838-$6839  UDP source port (requester)
     *   $683A-$683B  UDP dest port (4510)
     *   $683C-$683D  UDP length
     *   $683E-$683F  UDP checksum
     *   $6840+       UDP payload (our code)
     *
     * TX buffer (no length prefix, same layout minus 2):
     *   $6000-$6005  Dest MAC
     *   $6006-$600B  Source MAC
     *   $600C-$600D  EtherType
     *   $600E-$6035  IPv6 header (40 bytes)
     *   $6036-$603D  UDP header (8 bytes)
     *   $603E+       UDP payload (our response data)
     *
     * Response UDP payload:
     *   [0]     'R' (read response)
     *   [1]     Sequence number
     *   [2..5]  Source address (32-bit LE)
     *   [6..7]  Byte count (16-bit LE)
     *   [8..]   Data bytes
     */

    constexpr uint16_t tx = 0x6000;       /* TX buffer base */
    constexpr uint16_t rx = 0x6802;       /* RX frame start (after 2-byte length) */
    constexpr uint16_t tx_size_lo = 0xD6E2;
    constexpr uint16_t tx_size_hi = 0xD6E3;
    constexpr uint16_t tx_trigger = 0xD6E4;

    /* Offsets within the Ethernet frame */
    constexpr int OFF_DST_MAC    = 0;
    constexpr int OFF_SRC_MAC    = 6;
    constexpr int OFF_IPV6_PLEN  = 14 + 4;   /* payload length in IPv6 hdr */
    constexpr int OFF_IPV6_SRC   = 14 + 8;   /* source address */
    constexpr int OFF_IPV6_DST   = 14 + 24;  /* dest address */
    constexpr int OFF_UDP        = 14 + 40;   /* UDP header start */
    constexpr int OFF_UDP_SPORT  = OFF_UDP;
    constexpr int OFF_UDP_DPORT  = OFF_UDP + 2;
    constexpr int OFF_UDP_LEN    = OFF_UDP + 4;
    constexpr int OFF_UDP_CSUM   = OFF_UDP + 6;
    constexpr int OFF_UDP_DATA   = OFF_UDP + 8; /* = 62 = $3E */

    constexpr int ETH_HDR_SIZE = OFF_UDP_DATA; /* 62 bytes: ETH + IPv6 + UDP */
    constexpr int RESP_HDR_SIZE = 8; /* 'R', seq, addr[4], count[2] */
    uint16_t udp_payload_size = RESP_HDR_SIZE + count;
    uint16_t udp_total = 8 + udp_payload_size;  /* UDP header + payload */
    uint16_t frame_size = ETH_HDR_SIZE + udp_payload_size;

    std::vector<uint8_t> buf(384, 0);
    int pc = 0;

    /* Required: packet must start with LDA #imm ($A9) */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x00);

    /* Enable MEGA65 I/O — required for DMA and Ethernet register access */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x47);     /* LDA #$47 */
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD02F);  /* STA $D02F */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x53);     /* LDA #$53 */
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD02F);  /* STA $D02F */

    /* --- Step 1: Copy entire ETH+IPv6+UDP header from RX to TX --- */
    /* Copy 62 bytes from $6802 to $6000 using a loop */
    /* LDX #61 ($3D) */
    emit(buf, pc, 0xa2); emit(buf, pc, ETH_HDR_SIZE - 1);
    /* loop: LDA $6802,X */
    int copy_loop = pc;
    emit(buf, pc, 0xbd); emit16(buf, pc, rx);
    /* STA $6000,X */
    emit(buf, pc, 0x9d); emit16(buf, pc, tx);
    /* DEX */
    emit(buf, pc, 0xca);
    /* BPL loop */
    emit(buf, pc, 0x10); emit(buf, pc, static_cast<uint8_t>(copy_loop - pc));

    /* --- Step 2: Swap dest/src MAC (6 bytes each) --- */
    /* LDX #5 */
    emit(buf, pc, 0xa2); emit(buf, pc, 0x05);
    int swap_mac_loop = pc;
    /* LDA TX+OFF_DST_MAC,X  (currently = original dst = us) */
    emit(buf, pc, 0xbd); emit16(buf, pc, tx + OFF_DST_MAC);
    /* PHA */
    emit(buf, pc, 0x48);
    /* LDA TX+OFF_SRC_MAC,X  (currently = original src = requester) */
    emit(buf, pc, 0xbd); emit16(buf, pc, tx + OFF_SRC_MAC);
    /* STA TX+OFF_DST_MAC,X  (requester → dst) */
    emit(buf, pc, 0x9d); emit16(buf, pc, tx + OFF_DST_MAC);
    /* PLA */
    emit(buf, pc, 0x68);
    /* STA TX+OFF_SRC_MAC,X  (us → src) */
    emit(buf, pc, 0x9d); emit16(buf, pc, tx + OFF_SRC_MAC);
    /* DEX */
    emit(buf, pc, 0xca);
    /* BPL loop */
    emit(buf, pc, 0x10); emit(buf, pc, static_cast<uint8_t>(swap_mac_loop - pc));

    /* --- Step 3: Swap IPv6 src/dst addresses (16 bytes each) --- */
    /* LDX #15 */
    emit(buf, pc, 0xa2); emit(buf, pc, 0x0f);
    int swap_ip_loop = pc;
    emit(buf, pc, 0xbd); emit16(buf, pc, tx + OFF_IPV6_SRC);
    emit(buf, pc, 0x48);  /* PHA */
    emit(buf, pc, 0xbd); emit16(buf, pc, tx + OFF_IPV6_DST);
    emit(buf, pc, 0x9d); emit16(buf, pc, tx + OFF_IPV6_SRC);
    emit(buf, pc, 0x68);  /* PLA */
    emit(buf, pc, 0x9d); emit16(buf, pc, tx + OFF_IPV6_DST);
    emit(buf, pc, 0xca);  /* DEX */
    emit(buf, pc, 0x10); emit(buf, pc, static_cast<uint8_t>(swap_ip_loop - pc));

    /* --- Step 4: Swap UDP ports (2 bytes each) --- */
    /* LDX #1 */
    emit(buf, pc, 0xa2); emit(buf, pc, 0x01);
    int swap_port_loop = pc;
    emit(buf, pc, 0xbd); emit16(buf, pc, tx + OFF_UDP_SPORT);
    emit(buf, pc, 0x48);
    emit(buf, pc, 0xbd); emit16(buf, pc, tx + OFF_UDP_DPORT);
    emit(buf, pc, 0x9d); emit16(buf, pc, tx + OFF_UDP_SPORT);
    emit(buf, pc, 0x68);
    emit(buf, pc, 0x9d); emit16(buf, pc, tx + OFF_UDP_DPORT);
    emit(buf, pc, 0xca);
    emit(buf, pc, 0x10); emit(buf, pc, static_cast<uint8_t>(swap_port_loop - pc));

    /* --- Step 5: Update IPv6 payload length (big-endian) --- */
    emit(buf, pc, 0xa9); emit(buf, pc, (udp_total >> 8) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx + OFF_IPV6_PLEN);
    emit(buf, pc, 0xa9); emit(buf, pc, udp_total & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx + OFF_IPV6_PLEN + 1);

    /* --- Step 6: Update UDP length (big-endian) --- */
    emit(buf, pc, 0xa9); emit(buf, pc, (udp_total >> 8) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx + OFF_UDP_LEN);
    emit(buf, pc, 0xa9); emit(buf, pc, udp_total & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx + OFF_UDP_LEN + 1);

    /* --- Step 7: Zero UDP checksum (optional for IPv6 UDP per RFC 6935) --- */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x00);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx + OFF_UDP_CSUM);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx + OFF_UDP_CSUM + 1);

    /* --- Step 8: Write response header into UDP payload area --- */
    uint16_t payload_base = tx + OFF_UDP_DATA;

    /* 'R' response type */
    emit(buf, pc, 0xa9); emit(buf, pc, 'R');
    emit(buf, pc, 0x8d); emit16(buf, pc, payload_base + 0);

    /* Sequence number */
    emit(buf, pc, 0xa9); emit(buf, pc, seq);
    emit(buf, pc, 0x8d); emit16(buf, pc, payload_base + 1);

    /* Source address (4 bytes, little-endian) */
    emit(buf, pc, 0xa9); emit(buf, pc, address & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, payload_base + 2);
    emit(buf, pc, 0xa9); emit(buf, pc, (address >> 8) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, payload_base + 3);
    emit(buf, pc, 0xa9); emit(buf, pc, (address >> 16) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, payload_base + 4);
    emit(buf, pc, 0xa9); emit(buf, pc, (address >> 24) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, payload_base + 5);

    /* Byte count (2 bytes, little-endian) */
    emit(buf, pc, 0xa9); emit(buf, pc, count & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, payload_base + 6);
    emit(buf, pc, 0xa9); emit(buf, pc, (count >> 8) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, payload_base + 7);

    /* --- Step 9: DMA copy target memory → TX UDP payload data area --- */
    uint8_t src_mb = (address >> 20) & 0xff;
    emit(buf, pc, 0xa9); emit(buf, pc, src_mb);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD705); /* DMA source MB */
    emit(buf, pc, 0xa9); emit(buf, pc, 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD706); /* DMA dest MB = $FF */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x0d);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD702); /* DMA list bank */
    emit(buf, pc, 0xa9); emit(buf, pc, 0xe8);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD701); /* DMA list addr high */
    emit(buf, pc, 0xa9); emit(buf, pc, 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD704); /* DMA list MB */

    int dma_list_offset = pc + 5;
    uint8_t dma_list_lo = (0x40 + dma_list_offset) & 0xff;
    emit(buf, pc, 0xa9); emit(buf, pc, dma_list_lo);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD700); /* trigger DMA */

    /* Embedded DMA list */
    emit(buf, pc, 0x00);                   /* copy, no chain */
    emit16(buf, pc, count);                /* byte count */
    emit16(buf, pc, address & 0xffff);     /* source addr low 16 */
    emit(buf, pc, (address >> 16) & 0x0f); /* source bank */
    /* Dest: TX buffer's UDP payload data area.
     * TX buf at $DE000, UDP data at offset OFF_UDP_DATA + RESP_HDR_SIZE.
     * So dest = $E000 + OFF_UDP_DATA + RESP_HDR_SIZE */
    emit16(buf, pc, static_cast<uint16_t>(0xE000 + OFF_UDP_DATA + RESP_HDR_SIZE));
    emit(buf, pc, 0x0d);                   /* dest bank */
    emit16(buf, pc, 0x0000);               /* modulo */

    /* --- Step 10: Set TX frame size and trigger --- */
    emit(buf, pc, 0xa9); emit(buf, pc, frame_size & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_size_lo);
    emit(buf, pc, 0xa9); emit(buf, pc, (frame_size >> 8) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_size_hi);

    emit(buf, pc, 0xa9); emit(buf, pc, 0x01);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_trigger);

    /* RTS */
    emit(buf, pc, 0x60);

    buf.resize(pc);
    return buf;
}

/*
 * Memory fill routine -- uses DMA fill command.
 *
 * The DMA fill command on the F018B uses command byte $03 (fill, no chain).
 * The source address field becomes the fill value (low byte only).
 */
static std::vector<uint8_t> build_mem_fill_routine(uint32_t address,
                                                    uint16_t count,
                                                    uint8_t value,
                                                    uint8_t seq)
{
    std::vector<uint8_t> buf(128, 0);
    int pc = 0;

    /* Required LDA #imm prefix */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x00);

    /* Enable MEGA65 I/O */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x47);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD02F);
    emit(buf, pc, 0xa9); emit(buf, pc, 0x53);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD02F);

    uint8_t dest_mb = (address >> 20) & 0xff;

    /* Set DMA source MB (unused for fill, but required) */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x00);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD705);

    /* Set DMA dest MB */
    emit(buf, pc, 0xa9); emit(buf, pc, dest_mb);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD706);

    /* DMA list bank high */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x0d);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD702);

    /* DMA list addr high */
    emit(buf, pc, 0xa9); emit(buf, pc, 0xe8);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD701);

    /* DMA list MB */
    emit(buf, pc, 0xa9); emit(buf, pc, 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD704);

    /* DMA list low byte + trigger */
    int dma_list_offset = pc + 5;
    uint8_t dma_list_lo = (0x40 + dma_list_offset) & 0xff;
    emit(buf, pc, 0xa9); emit(buf, pc, dma_list_lo);
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD700);

    /* Embedded DMA list: fill command */
    emit(buf, pc, 0x03);                          /* DMA command: fill */
    emit16(buf, pc, count);                        /* byte count */
    emit16(buf, pc, static_cast<uint16_t>(value)); /* "source" = fill value */
    emit(buf, pc, 0x00);                           /* source bank (unused) */
    emit16(buf, pc, address & 0xffff);             /* dest address low 16 */
    emit(buf, pc, (address >> 16) & 0x0f);         /* dest bank */
    emit16(buf, pc, 0x0000);                       /* modulo */

    /* Store seq for debug (optional) */
    (void)seq;

    /* RTS */
    emit(buf, pc, 0x60);

    buf.resize(pc);
    return buf;
}

} // anonymous namespace

std::vector<uint8_t> build_dma_load(uint16_t addr, uint8_t bank, uint8_t mb,
                                     std::span<const uint8_t> data,
                                     uint8_t seq)
{
    std::vector<uint8_t> buf(DMA_PACKET_SIZE, 0);
    std::copy_n(dma_load_template, sizeof(dma_load_template), buf.begin());

    /* Patch destination address */
    buf[DMA_DEST_ADDR_OFFSET]     = static_cast<uint8_t>(addr & 0xff);
    buf[DMA_DEST_ADDR_OFFSET + 1] = static_cast<uint8_t>((addr >> 8) & 0xff);
    buf[DMA_DEST_BANK_OFFSET]     = bank;
    buf[DMA_DEST_MB_OFFSET]       = mb;

    /* Patch byte count */
    buf[DMA_BYTE_COUNT_OFFSET]     = static_cast<uint8_t>(data.size() & 0xff);
    buf[DMA_BYTE_COUNT_OFFSET + 1] = static_cast<uint8_t>((data.size() >> 8) & 0xff);

    /* Patch sequence number */
    buf[DMA_PACKET_NUMBER_OFFSET] = seq;

    /* Copy data payload */
    if (!data.empty())
        std::copy(data.begin(), data.end(), buf.begin() + DMA_DATA_OFFSET);

    return buf;
}

std::vector<uint8_t> build_done()
{
    std::vector<uint8_t> buf(DONE_PACKET_SIZE, 0);
    std::copy_n(done_template, sizeof(done_template), buf.begin());
    return buf;
}

std::vector<uint8_t> build_echo()
{
    /* Exact byte-for-byte copy of ethlet_echo from mega65-tools */
    static const uint8_t ethlet_echo[] = {
        0xa9,0x00,0xa9,0x47,0x8d,0x2f,0xd0,0xa9,0x53,0x8d,0x2f,0xd0,
        0xad,0xe1,0xd6,0x29,0x10,0xf0,0xf9,0x8d,0x07,0xd7,0x0b,0x80,
        0xff,0x81,0xff,0x00,0x04,0x3e,0x04,0x02,0xe8,0x8d,0x00,0xe8,
        0x8d,0x00,0x00,0x00,0x00,0x04,0x06,0x00,0x08,0xe8,0x8d,0x00,
        0xe8,0x8d,0x00,0x00,0x00,0x00,0x00,0x06,0x00,0xe9,0x36,0x8d,
        0x06,0xe8,0x8d,0x00,0x00,0x00,0xa9,0x68,0x5b,0xa5,0x38,0x85,
        0x38,0xa5,0x39,0x85,0x39,0xa5,0x3a,0x85,0x36,0xa5,0x3b,0x85,
        0x37,0xa9,0x3e,0x8d,0xe2,0xd6,0xa9,0x04,0x8d,0xe3,0xd6,0xa2,
        0x0f,0xb5,0x18,0x95,0x26,0xb5,0x28,0x95,0x16,0xca,0x10,0xf5,
        0xa9,0x01,0x8d,0xe4,0xd6,0xa9,0x00,0x5b,0x60
    };
    std::vector<uint8_t> buf(1024, 0);
    std::copy_n(ethlet_echo, sizeof(ethlet_echo), buf.begin());
    return buf;
}

std::vector<uint8_t> build_mem_read(uint32_t address, uint16_t count,
                                     uint8_t seq)
{
    return build_mem_read_routine(address, count, seq);
}

std::vector<uint8_t> build_mem_write(uint32_t address,
                                      std::span<const uint8_t> data,
                                      uint8_t seq)
{
    auto addr16 = static_cast<uint16_t>(address & 0xffff);
    auto bank   = static_cast<uint8_t>((address >> 16) & 0xff);
    auto mb     = static_cast<uint8_t>((address >> 20) & 0xff);
    return build_dma_load(addr16, bank, mb, data, seq);
}

std::vector<uint8_t> build_mem_fill(uint32_t address, uint16_t count,
                                     uint8_t value, uint8_t seq)
{
    return build_mem_fill_routine(address, count, value, seq);
}

bool parse_read_response(std::span<const uint8_t> packet,
                          uint32_t& address, uint8_t& seq,
                          std::vector<uint8_t>& data)
{
    /*
     * The response arrives as a UDP payload (kernel strips ETH+IPv6+UDP headers).
     * Layout:
     *   [0]     'R' (response type)
     *   [1]     Sequence number
     *   [2..5]  Source address (32-bit LE)
     *   [6..7]  Byte count (16-bit LE)
     *   [8..]   Data bytes
     */
    if (packet.size() < RESPONSE_HEADER_SIZE)
        return false;

    if (packet[0] != 'R')
        return false;

    seq = packet[1];
    address = packet[2]
            | (static_cast<uint32_t>(packet[3]) << 8)
            | (static_cast<uint32_t>(packet[4]) << 16)
            | (static_cast<uint32_t>(packet[5]) << 24);

    uint16_t count = packet[6] | (static_cast<uint16_t>(packet[7]) << 8);

    if (packet.size() < static_cast<size_t>(RESPONSE_HEADER_SIZE) + count)
        return false;

    data.assign(packet.begin() + RESPONSE_HEADER_SIZE,
                packet.begin() + RESPONSE_HEADER_SIZE + count);
    return true;
}

} // namespace etherdbg::protocol
