/*
 * etherdbg - MEGA65 Ethernet debug tool
 *
 * Communicates with a MEGA65 running the ETHLOAD.M65 listener
 * (activated by Shift+pound on the keyboard).
 *
 * Usage:
 *   etherdbg load <ip> <file.prg>            Load a PRG file
 *   etherdbg read <ip> <addr> [count]        Read memory (hex dump)
 *   etherdbg write <ip> <addr> <byte>...     Write bytes to memory
 *   etherdbg fill <ip> <addr> <count> <val>  Fill memory with value
 *   etherdbg peek <ip> <addr>                Read single byte
 *   etherdbg poke <ip> <addr> <val>          Write single byte
 */

#include <cstdlib>
#include <cstring>
#include <print>
#include <string_view>
#include <charconv>

#include "transport_udp.h"
#include "commands.h"
#include "protocol.h"

using namespace std::string_view_literals;

static uint32_t parse_hex(std::string_view s)
{
    /* Skip optional $ or 0x prefix */
    if (s.starts_with("0x") || s.starts_with("0X"))
        s.remove_prefix(2);
    else if (s.starts_with("$"))
        s.remove_prefix(1);

    uint32_t val = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val, 16);
    if (ec != std::errc{}) {
        std::println(stderr, "etherdbg: invalid hex value '{}'", s);
        std::exit(1);
    }
    return val;
}

static void hex_dump(uint32_t base_addr, std::span<const uint8_t> data)
{
    for (size_t i = 0; i < data.size(); i += 16) {
        std::print(":{:08X}:", base_addr + static_cast<uint32_t>(i));
        for (size_t j = 0; j < 16; j++) {
            if (i + j < data.size())
                std::print("{:02X}", data[i + j]);
            else
                std::print("  ");
        }
        std::print("  ");
        for (size_t j = 0; j < 16 && i + j < data.size(); j++) {
            char c = static_cast<char>(data[i + j]);
            std::print("{}", (c >= ' ' && c < 0x7f) ? c : '.');
        }
        std::println("");
    }
}

static void usage(std::string_view progname)
{
    std::println(stderr,
        "Usage:\n"
        "  {} load <ip> <file.prg>            Load a PRG file\n"
        "  {} read <ip> <addr> [count]        Read memory (default 256 bytes)\n"
        "  {} write <ip> <addr> <byte>...     Write hex bytes to memory\n"
        "  {} fill <ip> <addr> <count> <val>  Fill memory region\n"
        "  {} peek <ip> <addr>                Read single byte\n"
        "  {} poke <ip> <addr> <val>          Write single byte\n"
        "\n"
        "Addresses and values are in hex (optional $ or 0x prefix).\n"
        "\n"
        "Options:\n"
        "  -p <port>   UDP port (default: 4510)\n"
        "  -v          Verbose output\n"
        "  -q          Quiet (errors only)",
        progname, progname, progname, progname, progname, progname);
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

    /* All commands need at least an IP address */
    if (argidx >= argc && command != "help") {
        std::println(stderr, "etherdbg {}: requires <ip-address>", command);
        return 1;
    }

    auto create_transport = [&]() -> std::unique_ptr<etherdbg::Transport> {
        auto ip = std::string_view(argv[argidx++]);
        auto t = etherdbg::create_udp_transport(ip, port);
        if (!t) {
            std::println(stderr, "etherdbg: failed to create UDP transport");
            std::exit(1);
        }
        t->verbose = (verbose >= 2);
        return t;
    };

    if (command == "load") {
        if (argidx + 2 > argc) {
            std::println(stderr,
                         "etherdbg load: requires <ip-address> <file.prg>");
            return 1;
        }
        auto transport = create_transport();
        auto file = std::string_view(argv[argidx++]);

        if (verbose >= 1)
            std::println("etherdbg: loading '{}' to port {}", file, port);

        int ret = etherdbg::cmd_load_program(*transport, file, verbose >= 1);
        return ret == 0 ? 0 : 1;
    }

    if (command == "read") {
        if (argidx + 1 > argc) {
            std::println(stderr,
                         "etherdbg read: requires <ip> <addr> [count]");
            return 1;
        }
        auto transport = create_transport();
        auto addr = parse_hex(argv[argidx++]);
        uint32_t count = 256;
        if (argidx < argc)
            count = parse_hex(argv[argidx++]);

        auto data = etherdbg::cmd_read_memory(*transport, addr, count,
                                               verbose >= 1);
        if (data.empty())
            return 1;
        hex_dump(addr, data);
        return 0;
    }

    if (command == "peek") {
        if (argidx + 1 > argc) {
            std::println(stderr, "etherdbg peek: requires <ip> <addr>");
            return 1;
        }
        auto transport = create_transport();
        auto addr = parse_hex(argv[argidx++]);

        auto data = etherdbg::cmd_read_memory(*transport, addr, 1, false);
        if (data.empty())
            return 1;
        std::println("${:07X} = ${:02X} ({})", addr, data[0], data[0]);
        return 0;
    }

    if (command == "write") {
        if (argidx + 2 > argc) {
            std::println(stderr,
                         "etherdbg write: requires <ip> <addr> <byte>...");
            return 1;
        }
        auto transport = create_transport();
        auto addr = parse_hex(argv[argidx++]);
        std::vector<uint8_t> data;
        while (argidx < argc)
            data.push_back(static_cast<uint8_t>(parse_hex(argv[argidx++])));

        int ret = etherdbg::cmd_write_memory(*transport, addr, data,
                                              verbose >= 1);
        return ret == 0 ? 0 : 1;
    }

    if (command == "poke") {
        if (argidx + 2 > argc) {
            std::println(stderr,
                         "etherdbg poke: requires <ip> <addr> <val>");
            return 1;
        }
        auto transport = create_transport();
        auto addr = parse_hex(argv[argidx++]);
        auto val = static_cast<uint8_t>(parse_hex(argv[argidx++]));
        std::vector<uint8_t> data = {val};

        int ret = etherdbg::cmd_write_memory(*transport, addr, data, false);
        if (ret == 0 && verbose >= 1)
            std::println("${:07X} <- ${:02X}", addr, val);
        return ret == 0 ? 0 : 1;
    }

    if (command == "fill") {
        if (argidx + 3 > argc) {
            std::println(stderr,
                         "etherdbg fill: requires <ip> <addr> <count> <val>");
            return 1;
        }
        auto transport = create_transport();
        auto addr = parse_hex(argv[argidx++]);
        auto count = parse_hex(argv[argidx++]);
        auto val = static_cast<uint8_t>(parse_hex(argv[argidx++]));

        int ret = etherdbg::cmd_fill_memory(*transport, addr, count, val,
                                             verbose >= 1);
        return ret == 0 ? 0 : 1;
    }

    std::println(stderr, "Unknown command: {}", command);
    usage(argv[0]);
    return 1;
}
