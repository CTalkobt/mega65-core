/*
 * commands.h - High-level MEGA65 debug commands
 *
 * These operate on a Transport handle and use the protocol layer to
 * construct packets. Commands represent user-visible operations like
 * "load a PRG file" or (future) "read memory".
 */

#pragma once

#include "transport.h"
#include <string_view>

namespace etherdbg {

inline constexpr int DEFAULT_PACKET_DELAY_US = 150;
inline constexpr int DONE_REPEAT_COUNT = 10;

/* Timeout for waiting for a response from the MEGA65 (ms) */
inline constexpr int DEFAULT_RECV_TIMEOUT_MS = 2000;

/* Number of retries for memory read if no response */
inline constexpr int DEFAULT_READ_RETRIES = 3;

/*
 * Load a PRG file to the MEGA65.
 *
 * Reads the 2-byte load address header, then sends the file contents
 * in 1024-byte DMA load packets, followed by the "all done" packet.
 *
 * Returns 0 on success, -1 on error.
 */
int cmd_load_program(Transport& transport, std::string_view filename,
                     bool verbose);

/*
 * Read memory from the MEGA65 and return it as a byte vector.
 *
 * Sends a memory-read routine to execute on the MEGA65, which DMA-copies
 * the requested region into the Ethernet TX buffer and transmits it back.
 * Chunks reads larger than MAX_READ_SIZE into multiple requests.
 *
 * Returns the read data, or an empty vector on failure.
 */
std::vector<uint8_t> cmd_read_memory(Transport& transport,
                                      uint32_t address, uint32_t count,
                                      bool verbose);

/*
 * Write data to MEGA65 memory.
 *
 * Uses DMA load packets to copy data to the target address.
 * Chunks writes larger than MAX_CHUNK_SIZE into multiple packets.
 *
 * Returns 0 on success, -1 on error.
 */
int cmd_write_memory(Transport& transport,
                     uint32_t address, std::span<const uint8_t> data,
                     bool verbose);

/*
 * Fill a memory region on the MEGA65 with a byte value.
 *
 * Returns 0 on success, -1 on error.
 */
int cmd_fill_memory(Transport& transport,
                    uint32_t address, uint32_t count, uint8_t value,
                    bool verbose);

} // namespace etherdbg
