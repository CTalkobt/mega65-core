/*
 * commands.cpp - High-level MEGA65 debug commands
 */

#include <array>
#include <fstream>
#include <print>
#include <thread>
#include <chrono>

#include "commands.h"
#include "protocol.h"

namespace etherdbg {

int cmd_load_program(Transport& transport, std::string_view filename,
                     bool verbose)
{
    std::ifstream file(std::string(filename), std::ios::binary);
    if (!file) {
        std::println(stderr, "etherdbg: cannot open '{}'", filename);
        return -1;
    }

    /* Read 2-byte PRG load address */
    std::array<uint8_t, 2> hdr{};
    file.read(reinterpret_cast<char*>(hdr.data()), 2);
    if (file.gcount() < 2) {
        std::println(stderr, "etherdbg: failed to read load address from '{}'",
                     filename);
        return -1;
    }

    uint32_t address = hdr[0] | (hdr[1] << 8);
    if (verbose)
        std::println("Load address: ${:04X}", address);

    std::array<uint8_t, protocol::MAX_CHUNK_SIZE> filebuf{};
    int total_bytes = 0;
    uint8_t seq = 0;

    while (file.read(reinterpret_cast<char*>(filebuf.data()), filebuf.size())
           || file.gcount() > 0) {
        auto bytes = static_cast<int>(file.gcount());
        if (verbose)
            std::println("  Sending {} bytes -> ${:04X}", bytes, address);

        auto addr16 = static_cast<uint16_t>(address & 0xffff);
        auto bank   = static_cast<uint8_t>((address >> 16) & 0xff);
        auto mb     = static_cast<uint8_t>((address >> 20) & 0xff);

        auto packet = protocol::build_dma_load(
            addr16, bank, mb,
            std::span<const uint8_t>(filebuf.data(), bytes), seq);

        auto result = transport.send(packet);
        if (!result) {
            std::println(stderr, "etherdbg: send failed at offset {}: {}",
                         total_bytes, to_string(result.error()));
            return -1;
        }

        std::this_thread::sleep_for(
            std::chrono::microseconds(DEFAULT_PACKET_DELAY_US));
        seq++;
        address += bytes;
        total_bytes += bytes;
    }

    if (verbose)
        std::println("Sent {} bytes from '{}'", total_bytes, filename);

    /* Send "all done" packet multiple times for reliability */
    auto done_pkt = protocol::build_done();

    if (verbose)
        std::print("Sending completion signal");

    for (int i = 0; i < DONE_REPEAT_COUNT; i++) {
        transport.send(done_pkt);
        std::this_thread::sleep_for(
            std::chrono::microseconds(DEFAULT_PACKET_DELAY_US));
        if (verbose) {
            std::print(".");
            std::fflush(stdout);
        }
    }

    if (verbose)
        std::println(" done");

    return 0;
}

} // namespace etherdbg
