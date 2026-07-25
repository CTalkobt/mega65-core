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

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace etherdbg::protocol {

inline constexpr int MAX_CHUNK_SIZE = 1024;
inline constexpr int DMA_PACKET_SIZE = 128 + MAX_CHUNK_SIZE;
inline constexpr int DONE_PACKET_SIZE = 128;

/*
 * Build a DMA load packet that will copy payload data to the given
 * 28-bit MEGA65 address when executed on the target.
 */
std::vector<uint8_t> build_dma_load(uint16_t addr, uint8_t bank, uint8_t mb,
                                     std::span<const uint8_t> data,
                                     uint8_t seq);

/*
 * Build the "all done" packet that restores normal memory mapping
 * and returns control to the MEGA65.
 */
std::vector<uint8_t> build_done();

/*
 * Build the echo ethlet — identical to mega65-tools ethlet_echo.
 * When executed on the MEGA65, this echoes the entire received frame
 * back to the sender (with swapped MACs, IPs, and ports).
 * Used as a ping/ACK mechanism to confirm ETHLOAD is running.
 */
std::vector<uint8_t> build_echo();

/*
 * Build the reset-to-C64 ethlet (equivalent to etherload -4).
 * Patches: end_addr, do_run, cart_detect, d81, rom, video_mode, restore_prg.
 */
struct ResetC64Options {
    uint16_t end_address = 0;
    bool do_run = false;
    bool cart_detect = false;
    bool restore_prg = false;
    bool enable_default_rom_load = true;
    int video_mode = 0;   /* 0=unchanged, 1=PAL, -1=NTSC */
    std::string d81_filename;
};
std::vector<uint8_t> build_reset_c64(const ResetC64Options& opts = {});

/*
 * Build the reset-to-MEGA65 ethlet (equivalent to etherload -5).
 * Same patch fields as C64 reset but different ethlet.
 */
struct ResetM65Options {
    uint16_t end_address = 0;
    bool do_run = false;
    bool cart_detect = false;
    bool restore_prg = false;
    bool enable_default_rom_load = true;
    int video_mode = 0;
    std::string d81_filename;
};
std::vector<uint8_t> build_reset_m65(const ResetM65Options& opts = {});

/*
 * Build the jump ethlet (equivalent to etherload -j).
 * Jumps to the specified address after unmapping Ethernet buffers.
 */
std::vector<uint8_t> build_jump(uint16_t address,
                                 const std::string& d81_filename = {});

/*
 * Build the DMA load ethlet for transferring data to MEGA65 memory.
 * This is the data transfer packet used by etherload for file loading.
 */
struct DmaLoadOptions {
    uint32_t dest_address = 0;
    uint16_t byte_count = 0;
    bool rom_write_enable = false;
    uint16_t seq_num = 0;
};
std::vector<uint8_t> build_dma_load_ethlet(const DmaLoadOptions& opts,
                                            std::span<const uint8_t> data);

/*
 * Maximum bytes that can be read in a single memory-read packet.
 *
 * The response is sent as a proper IPv6 UDP packet back to the sender.
 * The 45GS02 routine copies the incoming IPv6+UDP headers from the RX
 * buffer, swaps src/dst, and places our payload in the UDP data area.
 *
 * UDP payload layout (what the host receives via recvfrom):
 *   [0]      Response type: 'R' = read response
 *   [1]      Sequence number
 *   [2..5]   Source address (32-bit, little-endian)
 *   [6..7]   Byte count (16-bit, little-endian)
 *   [8..]    Data bytes
 */
inline constexpr int RESPONSE_HEADER_SIZE = 8;
inline constexpr int MAX_READ_SIZE = 1024;

/*
 * Build a screen-save ethlet. DMA-copies screen RAM ($0400, 1000 bytes)
 * and colour RAM ($1F800, 1000 bytes) to upper memory ($10000).
 * Used to preserve screen contents during ETHLOAD operations.
 */
std::vector<uint8_t> build_screen_save();

/*
 * Build a screen-restore ethlet. DMA-copies screen RAM and colour RAM
 * back from upper memory ($10000) to their original locations.
 */
std::vector<uint8_t> build_screen_restore();

/*
 * Build a memory-read packet. When executed on the MEGA65, this routine:
 *   1. Copies the requester's MAC into the TX buffer as destination
 *   2. Sets our MAC as source
 *   3. Writes the etherdbg response header
 *   4. DMA-copies 'count' bytes from the target address into the TX payload
 *   5. Sets TX size and triggers transmit
 *
 * The host must call transport.recv() after sending to collect the response.
 */
std::vector<uint8_t> build_mem_read(uint32_t address, uint16_t count,
                                     uint8_t seq);

/*
 * Build a memory-write packet. This is a thin wrapper around build_dma_load
 * that accepts a full 28-bit address.
 */
std::vector<uint8_t> build_mem_write(uint32_t address,
                                      std::span<const uint8_t> data,
                                      uint8_t seq);

/*
 * Build a memory-fill packet. When executed on the MEGA65, this routine
 * uses DMA fill to set 'count' bytes at 'address' to 'value'.
 */
std::vector<uint8_t> build_mem_fill(uint32_t address, uint16_t count,
                                     uint8_t value, uint8_t seq);

/*
 * Parse a memory-read response packet received from the MEGA65.
 * Returns true if the packet is a valid etherdbg read response.
 * On success, fills in address, seq, and data.
 */
bool parse_read_response(std::span<const uint8_t> packet,
                          uint32_t& address, uint8_t& seq,
                          std::vector<uint8_t>& data);

} // namespace etherdbg::protocol
