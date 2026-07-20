/*
 * commands.h - High-level MEGA65 debug commands
 *
 * These operate on a transport handle and use the protocol layer to
 * construct packets. Commands represent user-visible operations like
 * "load a PRG file" or (future) "read memory".
 */

#ifndef ETHERDBG_COMMANDS_H
#define ETHERDBG_COMMANDS_H

#include "transport.h"

/* Inter-packet delay in microseconds */
#define CMD_DEFAULT_PACKET_DELAY_US 150

/* Number of times to send the "done" packet (for reliability) */
#define CMD_DONE_REPEAT_COUNT 10

/*
 * Load a PRG file to the MEGA65.
 *
 * Reads the 2-byte load address header, then sends the file contents
 * in 1024-byte DMA load packets, followed by the "all done" packet.
 *
 * t:         transport handle
 * filename:  path to .prg file
 * verbose:   print progress to stdout
 *
 * Returns 0 on success, -1 on error.
 */
int cmd_load_program(struct transport *t, const char *filename, int verbose);

#endif /* ETHERDBG_COMMANDS_H */
