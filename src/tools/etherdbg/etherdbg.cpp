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

#include <cstdlib>
#include <cstring>
#include <print>
#include <string_view>

#include "transport_udp.h"
#include "commands.h"

using namespace std::string_view_literals;

static void usage(std::string_view progname)
{
    std::println(stderr,
        "Usage:\n"
        "  {} load <ip-address> <file.prg>   Load a PRG file to MEGA65\n"
        "\n"
        "Options:\n"
        "  -p <port>   UDP port (default: 4510)\n"
        "  -v          Verbose output\n"
        "  -q          Quiet (errors only)",
        progname);
}

int main(int argc, char** argv)
{
    int port = etherdbg::DEFAULT_PORT;
    int verbose = 1;

    /* Parse leading options */
    int argidx = 1;
    while (argidx < argc && argv[argidx][0] == '-') {
        auto arg = std::string_view(argv[argidx]);
        if (arg == "-p" && argidx + 1 < argc) {
            port = std::atoi(argv[++argidx]);
        } else if (arg == "-v") {
            verbose = 2;
        } else if (arg == "-q") {
            verbose = 0;
        } else {
            std::println(stderr, "Unknown option: {}", arg);
            usage(argv[0]);
            return 1;
        }
        argidx++;
    }

    if (argidx >= argc) {
        usage(argv[0]);
        return 1;
    }

    auto command = std::string_view(argv[argidx++]);

    if (command == "load") {
        if (argidx + 2 > argc) {
            std::println(stderr,
                         "etherdbg load: requires <ip-address> <file.prg>");
            return 1;
        }
        auto ip   = std::string_view(argv[argidx++]);
        auto file = std::string_view(argv[argidx++]);

        auto transport = etherdbg::create_udp_transport(ip, port);
        if (!transport) {
            std::println(stderr, "etherdbg: failed to create UDP transport");
            return 1;
        }
        transport->verbose = (verbose >= 2);

        if (verbose >= 1)
            std::println("etherdbg: loading '{}' to {}:{}", file, ip, port);

        int ret = etherdbg::cmd_load_program(*transport, file, verbose >= 1);
        return ret == 0 ? 0 : 1;
    }

    std::println(stderr, "Unknown command: {}", command);
    usage(argv[0]);
    return 1;
}
