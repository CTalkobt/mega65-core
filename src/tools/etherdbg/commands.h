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

} // namespace etherdbg
