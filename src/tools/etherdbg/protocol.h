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

} // namespace etherdbg::protocol
