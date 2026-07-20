/*
 * etherdbg - MEGA65 Ethernet debug tool
 *
 * Communicates with a MEGA65 running the ETHLOAD.M65 listener
 * (activated by Shift+pound on the keyboard).
 *
 * Usage:
 *   etherdbg load <ip-address> <file.prg>     Load a PRG file
 *
 * Future commands:
 *   etherdbg read <ip-address> <addr> <len>   Read memory
 *   etherdbg write <ip-address> <addr> <data>  Write memory
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"
#include "transport_udp.h"
#include "commands.h"

static void usage(const char *progname)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s load <ip-address> <file.prg>   Load a PRG file to MEGA65\n"
        "\n"
        "Options:\n"
        "  -p <port>   UDP port (default: 4510)\n"
        "  -v          Verbose output\n"
        "  -q          Quiet (errors only)\n",
        progname);
}

int main(int argc, char **argv)
{
    int port = ETHERDBG_DEFAULT_PORT;
    int verbose = 1;

    /* Parse leading options */
    int argidx = 1;
    while (argidx < argc && argv[argidx][0] == '-') {
        if (strcmp(argv[argidx], "-p") == 0 && argidx + 1 < argc) {
            port = atoi(argv[++argidx]);
        } else if (strcmp(argv[argidx], "-v") == 0) {
            verbose = 2;
        } else if (strcmp(argv[argidx], "-q") == 0) {
            verbose = 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[argidx]);
            usage(argv[0]);
            return 1;
        }
        argidx++;
    }

    if (argidx >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *command = argv[argidx++];

    if (strcmp(command, "load") == 0) {
        if (argidx + 2 > argc) {
            fprintf(stderr, "etherdbg load: requires <ip-address> <file.prg>\n");
            return 1;
        }
        const char *ip = argv[argidx++];
        const char *file = argv[argidx++];

        struct transport *t = transport_udp_create(ip, port);
        if (!t) {
            fprintf(stderr, "etherdbg: failed to create UDP transport\n");
            return 1;
        }
        t->verbose = (verbose >= 2);

        if (verbose >= 1)
            printf("etherdbg: loading '%s' to %s:%d\n", file, ip, port);

        int ret = cmd_load_program(t, file, verbose >= 1);
        transport_close(t);
        return ret == 0 ? 0 : 1;
    }

    fprintf(stderr, "Unknown command: %s\n", command);
    usage(argv[0]);
    return 1;
}
