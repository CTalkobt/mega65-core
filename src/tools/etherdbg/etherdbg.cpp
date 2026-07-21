/*
 * etherdbg - MEGA65 Ethernet debug tool
 *
 * Communicates with a MEGA65 running the ETHLOAD.M65 listener
 * (activated by Shift+pound on the keyboard).
 *
 * The IP address is optional for all commands. If omitted, etherdbg
 * broadcasts to discover the MEGA65 on the local network.
 *
 * Usage:
 *   etherdbg load [ip] <file.prg>            Load a PRG file
 *   etherdbg read [ip] <addr> [count]        Read memory (hex dump)
 *   etherdbg write [ip] <addr> <byte>...     Write bytes to memory
 *   etherdbg fill [ip] <addr> <count> <val>  Fill memory with value
 *   etherdbg peek [ip] <addr>                Read single byte
 *   etherdbg poke [ip] <addr> <val>          Write single byte
 *   etherdbg screen [ip] [file.png]          Screenshot (ASCII + PNG)
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string_view>
#include <charconv>
#include <thread>

#include "transport_udp.h"
#include "commands.h"
#include "protocol.h"
#include "screen.h"

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

/*
 * Heuristic: does this string look like an IP/IPv6 address?
 * IPv6 link-local: contains ':' (e.g. "fe80::1234%eth0")
 * IPv4: digits-and-dots (e.g. "192.168.1.1")
 */
static bool looks_like_ip(std::string_view s)
{
    if (s.empty()) return false;
    /* IPv6 addresses always contain colons */
    if (s.find(':') != std::string_view::npos)
        return true;
    /* IPv4: digits and exactly 3 dots */
    int dots = 0;
    for (char c : s) {
        if (c == '.')
            dots++;
        else if (c < '0' || c > '9')
            return false;
    }
    return dots == 3;
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
        "  {} load [ip] <file.prg>            Load a PRG file\n"
        "  {} read [ip] <addr> [count]        Read memory (default 256 bytes)\n"
        "  {} write [ip] <addr> <byte>...     Write hex bytes to memory\n"
        "  {} fill [ip] <addr> <count> <val>  Fill memory region\n"
        "  {} peek [ip] <addr>                Read single byte\n"
        "  {} poke [ip] <addr> <val>          Write single byte\n"
        "  {} screen [ip] [file.png]          Screenshot (ASCII + PNG)\n"
        "\n"
        "If <ip> is omitted, broadcasts to auto-detect the MEGA65.\n"
        "Addresses and values are in hex. Use 0x prefix (e.g. 0xD020) or\n"
        "plain hex (e.g. D020). Avoid shell's $-prefix as bash interprets\n"
        "it as a variable (use '\\$D020' or single quotes if you must).\n"
        "\n"
        "Options:\n"
        "  -p <port>   UDP port (default: 4510)\n"
        "  -v          Verbose output\n"
        "  -vv         Packet hex dumps\n"
        "  -q          Quiet (errors only)",
        progname, progname, progname, progname, progname, progname, progname);
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
        } else if (arg == "-vv") {
            verbose = 3;
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

    /*
     * Try to consume an IP address from the next argument.
     * If it looks like an IP, use direct mode; otherwise broadcast.
     */
    auto create_transport = [&]() -> std::unique_ptr<etherdbg::Transport> {
        std::unique_ptr<etherdbg::Transport> t;

        if (argidx < argc && looks_like_ip(argv[argidx])) {
            auto ip = std::string_view(argv[argidx++]);
            if (verbose >= 1)
                std::println("etherdbg: connecting to {}", ip);
            t = etherdbg::create_udp_transport(ip, port);
        } else {
            if (verbose >= 1)
                std::println("etherdbg: discovering MEGA65 on local network...");
            t = etherdbg::create_udp_transport_auto(port, verbose >= 1);
        }

        if (!t) {
            std::println(stderr, "etherdbg: failed to create UDP transport");
            std::exit(1);
        }
        t->verbose = (verbose >= 2);
        t->trace = (verbose >= 3);

        if (!etherdbg::cmd_connect(*t, verbose >= 1)) {
            std::println(stderr, "etherdbg: failed to connect to MEGA65");
            std::exit(1);
        }
        return t;
    };

    if (command == "load") {
        if (argidx >= argc) {
            std::println(stderr, "etherdbg load: requires <file.prg>");
            return 1;
        }
        auto transport = create_transport();
        if (argidx >= argc) {
            std::println(stderr, "etherdbg load: requires <file.prg>");
            return 1;
        }
        auto file = std::string_view(argv[argidx++]);

        int ret = etherdbg::cmd_load_program(*transport, file, verbose >= 1);
        return ret == 0 ? 0 : 1;
    }

    if (command == "read") {
        if (argidx >= argc) {
            std::println(stderr, "etherdbg read: requires <addr> [count]");
            return 1;
        }
        auto transport = create_transport();
        if (argidx >= argc) {
            std::println(stderr, "etherdbg read: requires <addr>");
            return 1;
        }
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
        if (argidx >= argc) {
            std::println(stderr, "etherdbg peek: requires <addr>");
            return 1;
        }
        auto transport = create_transport();
        if (argidx >= argc) {
            std::println(stderr, "etherdbg peek: requires <addr>");
            return 1;
        }
        auto addr = parse_hex(argv[argidx++]);

        auto data = etherdbg::cmd_read_memory(*transport, addr, 1, false);
        if (data.empty())
            return 1;
        std::println("${:07X} = ${:02X} ({})", addr, data[0], data[0]);
        return 0;
    }

    if (command == "write") {
        if (argidx >= argc) {
            std::println(stderr, "etherdbg write: requires <addr> <byte>...");
            return 1;
        }
        auto transport = create_transport();
        if (argidx >= argc) {
            std::println(stderr, "etherdbg write: requires <addr> <byte>...");
            return 1;
        }
        auto addr = parse_hex(argv[argidx++]);
        if (argidx >= argc) {
            std::println(stderr, "etherdbg write: requires at least one byte");
            return 1;
        }
        std::vector<uint8_t> data;
        while (argidx < argc)
            data.push_back(static_cast<uint8_t>(parse_hex(argv[argidx++])));

        int ret = etherdbg::cmd_write_memory(*transport, addr, data,
                                              verbose >= 1);
        return ret == 0 ? 0 : 1;
    }

    if (command == "poke") {
        if (argidx >= argc) {
            std::println(stderr, "etherdbg poke: requires <addr> <val>");
            return 1;
        }
        auto transport = create_transport();
        if (argidx + 1 >= argc) {
            std::println(stderr, "etherdbg poke: requires <addr> <val>");
            return 1;
        }
        auto addr = parse_hex(argv[argidx++]);
        auto val = static_cast<uint8_t>(parse_hex(argv[argidx++]));
        std::vector<uint8_t> data = {val};

        int ret = etherdbg::cmd_write_memory(*transport, addr, data, false);
        if (ret == 0 && verbose >= 1)
            std::println("${:07X} <- ${:02X}", addr, val);
        return ret == 0 ? 0 : 1;
    }

    if (command == "fill") {
        if (argidx >= argc) {
            std::println(stderr, "etherdbg fill: requires <addr> <count> <val>");
            return 1;
        }
        auto transport = create_transport();
        if (argidx + 2 >= argc) {
            std::println(stderr, "etherdbg fill: requires <addr> <count> <val>");
            return 1;
        }
        auto addr = parse_hex(argv[argidx++]);
        auto count = parse_hex(argv[argidx++]);
        auto val = static_cast<uint8_t>(parse_hex(argv[argidx++]));

        int ret = etherdbg::cmd_fill_memory(*transport, addr, count, val,
                                             verbose >= 1);
        return ret == 0 ? 0 : 1;
    }

    if (command == "screen") {
        auto transport = create_transport();
        std::string_view png_file;
        if (argidx < argc)
            png_file = argv[argidx++];

        int ret = etherdbg::cmd_screen_shot(*transport, png_file,
                                             verbose >= 1);
        return ret == 0 ? 0 : 1;
    }

    if (command == "reset64" || command == "-4") {
        auto transport = create_transport();
        auto pkt = etherdbg::protocol::build_reset_c64();
        std::println("Resetting MEGA65 to C64 mode...");
        transport->send(pkt);
        return 0;
    }

    if (command == "reset65" || command == "-5") {
        auto transport = create_transport();
        auto pkt = etherdbg::protocol::build_reset_m65();
        std::println("Resetting MEGA65 to MEGA65 mode...");
        transport->send(pkt);
        return 0;
    }

    if (command == "echo") {
        auto transport = create_transport();

        auto echo_pkt = etherdbg::protocol::build_echo();

        std::println("Sending echo ethlet (identical to mega65-tools)...");
        for (int i = 0; i < 5; i++) {
            auto result = transport->send(echo_pkt);
            if (!result) {
                std::println(stderr, "etherdbg: send failed: {}",
                             etherdbg::to_string(result.error()));
                return 1;
            }
            if (verbose >= 1)
                std::println("  Sent echo #{}", i + 1);

            /* Check for echo response */
            auto recv_result = transport->recv(2048, 1000);
            if (recv_result) {
                std::println("  Got response: {} bytes", recv_result->size());
                if (verbose >= 2) {
                    std::print("  Data: ");
                    for (size_t j = 0; j < std::min(recv_result->size(), size_t{32}); j++)
                        std::print("{:02X} ", (*recv_result)[j]);
                    std::println("");
                }
                std::println("ETHLOAD is responding!");
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::println("No echo response received.");
        return 1;
    }

    if (command == "ping") {
        auto transport = create_transport();  /* includes echo handshake */

        std::vector<uint8_t> ping_pkt = {
            0xa9, 0x00,             /* LDA #$00 (required $A9 prefix) */
            0xa9, 0x47,             /* LDA #$47  ; enable MEGA65 I/O */
            0x8d, 0x2f, 0xd0,       /* STA $D02F */
            0xa9, 0x53,             /* LDA #$53 */
            0x8d, 0x2f, 0xd0,       /* STA $D02F */
            0xee, 0x20, 0xd0,       /* INC $D020 (change border colour) */
            0x60                    /* RTS */
        };

        std::println("Sending ping (INC $D020) — watch the MEGA65 border...");
        for (int i = 0; i < 20; i++) {
            transport->send(ping_pkt);
            if (verbose >= 1)
                std::println("  Sent ping #{}", i + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::println("If the border changed colour, ETHLOAD executed our code.");
        return 0;
    }

    std::println(stderr, "Unknown command: {}", command);
    usage(argv[0]);
    return 1;
}
