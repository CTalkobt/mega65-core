/*
 * etherdbg - MEGA65 Ethernet debug/load tool
 *
 * Compatible with mega65-tools etherload CLI options.
 *
 * Usage:
 *   etherdbg [options] [prgname]
 *
 * Examples:
 *   etherdbg -5                    Reset to MEGA65 mode
 *   etherdbg -4                    Reset to C64 mode
 *   etherdbg game.prg              Load PRG, auto-detect C64/M65 mode
 *   etherdbg -r game.prg           Load and RUN
 *   etherdbg -j 2000 loader.prg    Load and jump to $2000
 *   etherdbg -b 0800 data.bin      Load binary at $0800
 *   etherdbg -D                    Discover MEGA65 on network
 *   etherdbg --pal -5              Reset to M65 in PAL mode
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <print>
#include <string>
#include <string_view>
#include <charconv>
#include <thread>

#include "transport_udp.h"
#include "commands.h"
#include "protocol.h"
#include "screen.h"

static uint32_t parse_hex(const char* s)
{
    std::string_view sv(s);
    if (sv.starts_with("0x") || sv.starts_with("0X"))
        sv.remove_prefix(2);
    else if (sv.starts_with("$"))
        sv.remove_prefix(1);

    uint32_t val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val, 16);
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

static void usage(const char* progname)
{
    std::println(stderr,
        "MEGA65 Ethernet Debug/Load Tool\n"
        "\n"
        "Usage: {} [options] [prgname]\n"
        "\n"
        "Connection:\n"
        "  -D, --discover        Discover MEGA65 on network and exit\n"
        "  -i, --ip <addr>       IPv6 address (e.g. fe80::1234%%eth0)\n"
        "\n"
        "Loading:\n"
        "  -r, --run             Auto-RUN after loading\n"
        "  -R, --rom <file>      Upload ROM file\n"
        "  -b, --bin <addr>      Load as binary at hex address\n"
        "  -o, --offset <bytes>  Skip first N bytes (hex) of file\n"
        "\n"
        "Reset mode:\n"
        "  -4, --c64mode         Reset to C64 mode after transfer\n"
        "  -5, --m65mode         Reset to MEGA65 mode after transfer\n"
        "  -j, --jump <addr>     Jump to hex address after loading\n"
        "  --halt                Halt after transfer\n"
        "\n"
        "Options:\n"
        "  -m, --mount <d81>     Mount D81 image from SD card\n"
        "  --cart-detect         Enable cartridge signature detection\n"
        "  --pal                 Set PAL video mode on reset\n"
        "  --ntsc                Set NTSC video mode on reset\n"
        "\n"
        "Debug commands:\n"
        "  --ping                Send INC $D020 (border flash test)\n"
        "  --echo                Send echo ethlet (connectivity test)\n"
        "  --read <addr> [n]     Read memory (hex dump, default 256)\n"
        "  --write <addr> <b>..  Write hex bytes to memory\n"
        "  --peek <addr>         Read single byte\n"
        "  --poke <addr> <val>   Write single byte\n"
        "  --fill <a> <n> <val>  Fill memory\n"
        "  --screen [file.png]   Screenshot\n"
        "\n"
        "Verbosity:\n"
        "  -v, --verbose         Verbose output\n"
        "  -vv                   Packet hex dumps\n"
        "  -q, --quiet           Errors only\n"
        "  -h, --help            This help\n"
        "\n"
        "Addresses are hex. Use 0x prefix or plain hex (not $, bash conflicts).",
        progname);
}

int main(int argc, char** argv)
{
    int port = etherdbg::DEFAULT_PORT;
    int verbose = 1;
    std::string ip_address;
    std::string filename;
    std::string rom_file;
    std::string d81_image;
    bool discover_only = false;
    bool reset64 = false;
    bool reset65 = false;
    bool do_run = false;
    bool halt = false;
    bool do_jump = false;
    uint32_t jump_addr = 0;
    bool use_binary = false;
    uint32_t bin_load_addr = 0;
    int file_offset = 0;
    bool cart_detect = false;
    int video_mode = 0;  /* 0=unchanged, 1=PAL, -1=NTSC */

    /* Debug commands */
    bool do_ping = false;
    bool do_echo = false;
    bool do_read = false;
    bool do_write = false;
    bool do_peek = false;
    bool do_poke = false;
    bool do_fill = false;
    bool do_screen = false;
    std::string screen_file;
    uint32_t dbg_addr = 0;
    uint32_t dbg_count = 256;
    uint8_t dbg_val = 0;
    std::vector<uint8_t> dbg_bytes;
    bool no_hyperrupt = false;

    static struct option long_opts[] = {
        {"help",        no_argument,       nullptr, 'h'},
        {"discover",    no_argument,       nullptr, 'D'},
        {"ip",          required_argument, nullptr, 'i'},
        {"run",         no_argument,       nullptr, 'r'},
        {"rom",         required_argument, nullptr, 'R'},
        {"c64mode",     no_argument,       nullptr, '4'},
        {"m65mode",     no_argument,       nullptr, '5'},
        {"jump",        required_argument, nullptr, 'j'},
        {"bin",         required_argument, nullptr, 'b'},
        {"offset",      required_argument, nullptr, 'o'},
        {"mount",       required_argument, nullptr, 'm'},
        {"verbose",     no_argument,       nullptr, 'v'},
        {"quiet",       no_argument,       nullptr, 'q'},
        {"halt",        no_argument,       nullptr, 'H'},
        {"cart-detect", no_argument,       nullptr, 'C'},
        {"pal",         no_argument,       nullptr, 'P'},
        {"ntsc",        no_argument,       nullptr, 'N'},
        {"no-hyperrupt",no_argument,       nullptr, 'X'},
        {"ping",        no_argument,       nullptr, 1001},
        {"echo",        no_argument,       nullptr, 1002},
        {"read",        required_argument, nullptr, 1003},
        {"write",       required_argument, nullptr, 1004},
        {"peek",        required_argument, nullptr, 1005},
        {"poke",        required_argument, nullptr, 1006},
        {"fill",        required_argument, nullptr, 1007},
        {"screen",      optional_argument, nullptr, 1008},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hDi:rR:45j:b:o:m:vq", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'h': usage(argv[0]); return 0;
        case 'D': discover_only = true; break;
        case 'i': ip_address = optarg; break;
        case 'r': do_run = true; break;
        case 'R': rom_file = optarg; break;
        case '4': reset64 = true; break;
        case '5': reset65 = true; break;
        case 'j': do_jump = true; jump_addr = parse_hex(optarg); break;
        case 'b': use_binary = true; bin_load_addr = parse_hex(optarg); break;
        case 'o': file_offset = parse_hex(optarg); break;
        case 'm': d81_image = optarg; break;
        case 'v':
            if (verbose == 2) verbose = 3;  /* -v -v = -vv */
            else verbose = 2;
            break;
        case 'q': verbose = 0; break;
        case 'H': halt = true; break;
        case 'C': cart_detect = true; break;
        case 'P': video_mode = 1; break;
        case 'N': video_mode = -1; break;
        case 'X': no_hyperrupt = true; break;
        case 1001: do_ping = true; break;
        case 1002: do_echo = true; break;
        case 1003: do_read = true; dbg_addr = parse_hex(optarg); break;
        case 1004: do_write = true; dbg_addr = parse_hex(optarg); break;
        case 1005: do_peek = true; dbg_addr = parse_hex(optarg); break;
        case 1006: do_poke = true; dbg_addr = parse_hex(optarg); break;
        case 1007: do_fill = true; dbg_addr = parse_hex(optarg); break;
        case 1008: do_screen = true; if (optarg) screen_file = optarg; break;
        default: usage(argv[0]); return 1;
        }
    }

    /* Parse extra positional args for debug commands BEFORE filename,
     * since they consume args that would otherwise be treated as filenames */
    if (do_read && optind < argc)
        dbg_count = parse_hex(argv[optind++]);
    if (do_write) {
        while (optind < argc)
            dbg_bytes.push_back(static_cast<uint8_t>(parse_hex(argv[optind++])));
    }
    if (do_poke && optind < argc)
        dbg_val = static_cast<uint8_t>(parse_hex(argv[optind++]));
    if (do_fill) {
        if (optind < argc) dbg_count = parse_hex(argv[optind++]);
        if (optind < argc) dbg_val = static_cast<uint8_t>(parse_hex(argv[optind++]));
    }

    /* Remaining positional arg = PRG filename */
    if (optind < argc) {
        filename = argv[optind++];
        /* Check file exists early, before connecting */
        if (!std::filesystem::exists(filename)) {
            std::println(stderr, "etherdbg: file not found: '{}'", filename);
            return 1;
        }
    }

    /* --- Nothing to do? Show help --- */
    bool has_action = discover_only || reset64 || reset65 || do_jump || halt
                    || do_run || do_ping || do_echo || do_read || do_write
                    || do_peek || do_poke || do_fill || do_screen
                    || !filename.empty() || !rom_file.empty();
    if (!has_action) {
        usage(argv[0]);
        return 1;
    }

    /* --- Discover only --- */
    if (discover_only) {
        auto addr = etherdbg::discover_mega65(
            etherdbg::DEFAULT_DISCOVER_TIMEOUT_MS, true);
        if (addr.empty()) return 1;
        std::println("{}", addr);
        return 0;
    }

    /* --- Validate options --- */
    if (reset64 && reset65) {
        std::println(stderr, "etherdbg: -4 and -5 cannot both be specified");
        return 1;
    }
    if (video_mode != 0 && !reset64 && !reset65) {
        std::println(stderr, "etherdbg: --pal/--ntsc require -4 or -5");
        return 1;
    }
    if (do_run && filename.empty()) {
        std::println(stderr, "etherdbg: --run requires a filename");
        return 1;
    }

    /* Auto-detect mode from load address if no mode specified */
    if (!filename.empty() && !reset64 && !reset65 && !do_jump && !halt) {
        /* Will auto-detect after reading PRG header */
    }

    /* --- Create transport --- */
    std::unique_ptr<etherdbg::Transport> transport;
    if (!ip_address.empty()) {
        if (verbose >= 1)
            std::println("etherdbg: connecting to {}", ip_address);
        transport = etherdbg::create_udp_transport(ip_address, port);
    } else {
        if (verbose >= 1)
            std::println("etherdbg: discovering MEGA65...");
        transport = etherdbg::create_udp_transport_auto(port, verbose >= 1);
    }
    if (!transport) {
        std::println(stderr, "etherdbg: failed to create transport");
        return 1;
    }
    transport->verbose = (verbose >= 2);
    transport->trace = (verbose >= 3);

    if (!etherdbg::cmd_connect(*transport, verbose >= 1, no_hyperrupt)) {
        std::println(stderr, "etherdbg: failed to connect to MEGA65");
        return 1;
    }

    /* --- Debug commands (no file loading) --- */
    if (do_ping) {
        std::vector<uint8_t> ping_pkt = {
            0xa9, 0x00, 0xa9, 0x47, 0x8d, 0x2f, 0xd0,
            0xa9, 0x53, 0x8d, 0x2f, 0xd0,
            0xee, 0x20, 0xd0, 0x60
        };
        std::println("Sending ping...");
        for (int i = 0; i < 20; i++) {
            transport->send(ping_pkt);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        etherdbg::cmd_restore_screen(*transport, verbose >= 1);
        return 0;
    }

    if (do_echo) {
        auto echo_pkt = etherdbg::protocol::build_echo();
        std::println("Sending echo...");
        transport->send(echo_pkt);
        auto resp = transport->recv(2048, 2000);
        etherdbg::cmd_restore_screen(*transport, verbose >= 1);
        if (resp) {
            std::println("Echo response: {} bytes", resp->size());
            return 0;
        }
        std::println(stderr, "No echo response.");
        return 1;
    }

    if (do_peek) {
        auto data = etherdbg::cmd_read_memory(*transport, dbg_addr, 1, false);
        if (data.empty()) return 1;
        std::println("${:07X} = ${:02X} ({})", dbg_addr, data[0], data[0]);
        return 0;
    }

    if (do_read) {
        auto data = etherdbg::cmd_read_memory(*transport, dbg_addr, dbg_count,
                                               verbose >= 1);
        if (data.empty()) return 1;
        hex_dump(dbg_addr, data);
        return 0;
    }

    if (do_write) {
        if (dbg_bytes.empty()) {
            std::println(stderr, "etherdbg: --write requires data bytes");
            return 1;
        }
        return etherdbg::cmd_write_memory(*transport, dbg_addr, dbg_bytes,
                                           verbose >= 1) == 0 ? 0 : 1;
    }

    if (do_poke) {
        etherdbg::protocol::DmaLoadOptions opts;
        opts.dest_address = dbg_addr;
        opts.byte_count = 1;
        opts.rom_write_enable = true;
        opts.seq_num = 0;
        std::vector<uint8_t> data = {dbg_val};
        auto pkt = etherdbg::protocol::build_dma_load_ethlet(opts, data);
        auto result = transport->send(pkt);
        if (!result) {
            std::println(stderr, "etherdbg: send failed");
            return 1;
        }
        if (verbose >= 1)
            std::println("${:07X} <- ${:02X}", dbg_addr, dbg_val);
        return 0;
    }

    if (do_fill) {
        return etherdbg::cmd_fill_memory(*transport, dbg_addr, dbg_count,
                                          dbg_val, verbose >= 1) == 0 ? 0 : 1;
    }

    if (do_screen) {
        return etherdbg::cmd_screen_shot(*transport, screen_file,
                                          verbose >= 1) == 0 ? 0 : 1;
    }

    /* --- File loading --- */
    int end_address = 0;

    if (!filename.empty()) {
        uint32_t load_addr = bin_load_addr;
        bool use_header = !use_binary;

        /* If using PRG header, peek at it to auto-detect mode */
        if (use_header && !reset64 && !reset65 && !do_jump && !halt) {
            std::ifstream f(filename, std::ios::binary);
            if (f) {
                uint8_t hdr[2];
                f.read(reinterpret_cast<char*>(hdr), 2);
                if (f.gcount() == 2) {
                    uint16_t addr = hdr[0] | (hdr[1] << 8);
                    if (addr == 0x0801) {
                        if (verbose >= 1) std::println("PRG is C64 @0801");
                        reset64 = true;
                    } else if (addr == 0x2001) {
                        if (verbose >= 1) std::println("PRG is MEGA65 @2001");
                        reset65 = true;
                    } else {
                        std::println(stderr,
                            "etherdbg: can't determine mode from load address ${:04X}. Use -4 or -5.",
                            addr);
                        return 1;
                    }
                }
            }
        }

        /* Enable ROM writes for file loading */
        end_address = etherdbg::cmd_load_file(*transport, filename,
            load_addr, use_header, file_offset, true, verbose >= 1);
        if (end_address < 0) return 1;
    }

    if (!rom_file.empty()) {
        int ret = etherdbg::cmd_load_file(*transport, rom_file,
            0x20000, false, 0, true, verbose >= 1);
        if (ret < 0) return 1;
    }

    /* --- Reset/jump/halt --- */
    if (reset64) {
        if (verbose >= 1) std::println("Resetting to C64 mode...");
        etherdbg::protocol::ResetC64Options opts;
        opts.end_address = end_address;
        opts.do_run = do_run;
        opts.cart_detect = cart_detect;
        opts.restore_prg = !filename.empty();
        opts.d81_filename = d81_image;
        opts.video_mode = video_mode;
        transport->send(etherdbg::protocol::build_reset_c64(opts));
    } else if (reset65) {
        if (verbose >= 1) std::println("Resetting to MEGA65 mode...");
        etherdbg::protocol::ResetM65Options opts;
        opts.end_address = end_address;
        opts.do_run = do_run;
        opts.cart_detect = cart_detect;
        opts.restore_prg = !filename.empty();
        opts.d81_filename = d81_image;
        opts.video_mode = video_mode;
        transport->send(etherdbg::protocol::build_reset_m65(opts));
    } else if (do_jump) {
        if (verbose >= 1) std::println("Jumping to ${:04X}...", jump_addr);
        transport->send(etherdbg::protocol::build_jump(
            static_cast<uint16_t>(jump_addr), d81_image));
    } else if (halt) {
        if (verbose >= 1) std::println("Halting (waiting for next transfer).");
        /* Don't send any reset — ETHLOAD stays running */
    }

    return 0;
}
