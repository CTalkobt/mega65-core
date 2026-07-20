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
     * TX buffer layout we're building at $6000:
     *   [00-05]  Dest MAC (requester)
     *   [06-0B]  Source MAC (ours)
     *   [0C-0D]  EtherType: $65, $02 (big-endian on wire)
     *   [0E]     'R' (read response)
     *   [0F]     Sequence number
     *   [10-13]  Source address (little-endian)
     *   [14-15]  Byte count (little-endian)
     *   [16..]   Data
     *
     * Total TX frame size = 0x16 + count
     */
    constexpr uint16_t tx_base = 0x6000;
    constexpr uint16_t rx_src_mac = 0x6808;  /* requester's MAC in RX frame */
    constexpr uint16_t our_mac_reg = 0xD6E9; /* our MAC in I/O registers */
    constexpr uint16_t tx_size_lo = 0xD6E2;
    constexpr uint16_t tx_size_hi = 0xD6E3;
    constexpr uint16_t tx_trigger = 0xD6E4;
    constexpr int header_size = 0x16;
    uint16_t frame_size = header_size + count;

    std::vector<uint8_t> buf(256, 0);  /* routine + DMA list, will resize */
    int pc = 0;

    /* Required: packet must start with LDA #imm ($A9) */
    emit(buf, pc, 0xa9);  /* LDA #$00 */
    emit(buf, pc, 0x00);

    /* --- Copy requester's MAC ($6808-$680D) → TX dest MAC ($6000-$6005) --- */
    /* LDX #5 */
    emit(buf, pc, 0xa2); emit(buf, pc, 0x05);
    /* loop: LDA $6808,X */
    int mac_loop = pc;
    emit(buf, pc, 0xbd); emit16(buf, pc, rx_src_mac);
    /* STA $6000,X */
    emit(buf, pc, 0x9d); emit16(buf, pc, tx_base);
    /* DEX */
    emit(buf, pc, 0xca);
    /* BPL loop */
    emit(buf, pc, 0x10); emit(buf, pc, static_cast<uint8_t>(mac_loop - pc));

    /* --- Copy our MAC ($D6E9-$D6EE) → TX source MAC ($6006-$600B) --- */
    /* LDX #5 */
    emit(buf, pc, 0xa2); emit(buf, pc, 0x05);
    /* loop: LDA $D6E9,X */
    int mac2_loop = pc;
    emit(buf, pc, 0xbd); emit16(buf, pc, our_mac_reg);
    /* STA $6006,X */
    emit(buf, pc, 0x9d); emit16(buf, pc, tx_base + 6);
    /* DEX */
    emit(buf, pc, 0xca);
    /* BPL loop */
    emit(buf, pc, 0x10); emit(buf, pc, static_cast<uint8_t>(mac2_loop - pc));

    /* --- Write EtherType $6502 (big-endian on wire: $65, $02) --- */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x65);     /* LDA #$65 */
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x0C);  /* STA $600C */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x02);     /* LDA #$02 */
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x0D);  /* STA $600D */

    /* --- Response type 'R' --- */
    emit(buf, pc, 0xa9); emit(buf, pc, 'R');
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x0E);

    /* --- Sequence number --- */
    emit(buf, pc, 0xa9); emit(buf, pc, seq);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x0F);

    /* --- Source address (4 bytes, little-endian) --- */
    emit(buf, pc, 0xa9); emit(buf, pc, address & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x10);
    emit(buf, pc, 0xa9); emit(buf, pc, (address >> 8) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x11);
    emit(buf, pc, 0xa9); emit(buf, pc, (address >> 16) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x12);
    emit(buf, pc, 0xa9); emit(buf, pc, (address >> 24) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x13);

    /* --- Byte count (2 bytes, little-endian) --- */
    emit(buf, pc, 0xa9); emit(buf, pc, count & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x14);
    emit(buf, pc, 0xa9); emit(buf, pc, (count >> 8) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_base + 0x15);

    /* --- DMA copy: source memory → TX buffer data area ($6016+) --- */
    /* Set DMA source MB */
    uint8_t src_mb = (address >> 20) & 0xff;
    emit(buf, pc, 0xa9); emit(buf, pc, src_mb);   /* LDA #src_mb */
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD705); /* STA $D705 */

    /* Set DMA dest MB = $FF (TX buffer is in $FFDE000) */
    emit(buf, pc, 0xa9); emit(buf, pc, 0xff);     /* LDA #$FF */
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD706); /* STA $D706 */

    /* Set DMA list address: bank high byte */
    emit(buf, pc, 0xa9); emit(buf, pc, 0x0d);     /* LDA #$0D */
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD702); /* STA $D702 */

    /* DMA list address high byte (will be patched with actual list position) */
    emit(buf, pc, 0xa9); emit(buf, pc, 0xe8);     /* LDA #$E8 */
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD701); /* STA $D701 */

    /* DMA list MB = $FF */
    emit(buf, pc, 0xa9); emit(buf, pc, 0xff);     /* LDA #$FF */
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD704); /* STA $D704 */

    /* Record where the DMA list will be, so we can set the trigger address */
    int dma_list_offset = pc + 5;  /* after the STA $D700 instruction */
    /* Compute the low byte of the DMA list's address in the ETH RX buffer.
     * Our code starts at $6840 (packet payload), so the DMA list at
     * offset 'dma_list_offset' lives at $6840 + dma_list_offset.
     * The true address is $FFDE840 + dma_list_offset.
     * Low byte for $D700 = ($40 + dma_list_offset) & 0xFF */
    uint8_t dma_list_lo = (0x40 + dma_list_offset) & 0xff;

    /* Trigger DMA by writing list low byte to $D700 */
    emit(buf, pc, 0xa9); emit(buf, pc, dma_list_lo); /* LDA #list_lo */
    emit(buf, pc, 0x8d); emit16(buf, pc, 0xD700);    /* STA $D700 = trigger */

    /* --- Embedded DMA list (F018B format) --- */
    /* DMA command: copy, no chain */
    emit(buf, pc, 0x00);
    /* Byte count */
    emit16(buf, pc, count);
    /* Source address low 16 bits */
    emit16(buf, pc, address & 0xffff);
    /* Source bank */
    emit(buf, pc, (address >> 16) & 0x0f);
    /* Dest address: $E016 (TX buffer $DE000 + $16 = header offset) */
    emit16(buf, pc, 0xE000 + header_size);
    /* Dest bank: $0D */
    emit(buf, pc, 0x0d);
    /* Modulo (unused) */
    emit16(buf, pc, 0x0000);

    /* --- After DMA: set TX frame size and trigger --- */
    emit(buf, pc, 0xa9); emit(buf, pc, frame_size & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_size_lo);
    emit(buf, pc, 0xa9); emit(buf, pc, (frame_size >> 8) & 0xff);
    emit(buf, pc, 0x8d); emit16(buf, pc, tx_size_hi);

    /* Trigger TX: write $01 to $D6E4 */
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
    /* Minimum: 14 bytes ETH header + 8 bytes our header = 22 */
    if (packet.size() < RESPONSE_HEADER_SIZE)
        return false;

    /* Check EtherType: $65, $02 (big-endian) */
    if (packet[12] != 0x65 || packet[13] != 0x02)
        return false;

    /* Check response type */
    if (packet[14] != 'R')
        return false;

    seq = packet[15];
    address = packet[16]
            | (static_cast<uint32_t>(packet[17]) << 8)
            | (static_cast<uint32_t>(packet[18]) << 16)
            | (static_cast<uint32_t>(packet[19]) << 24);

    uint16_t count = packet[20] | (static_cast<uint16_t>(packet[21]) << 8);

    if (packet.size() < static_cast<size_t>(RESPONSE_HEADER_SIZE) + count)
        return false;

    data.assign(packet.begin() + RESPONSE_HEADER_SIZE,
                packet.begin() + RESPONSE_HEADER_SIZE + count);
    return true;
}

} // namespace etherdbg::protocol
