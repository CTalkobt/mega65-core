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

std::vector<uint8_t> cmd_read_memory(Transport& transport,
                                      uint32_t address, uint32_t count,
                                      bool verbose)
{
    std::vector<uint8_t> result;
    result.reserve(count);
    uint8_t seq = 0;

    while (result.size() < count) {
        auto remaining = static_cast<uint16_t>(
            std::min(static_cast<uint32_t>(protocol::MAX_READ_SIZE),
                     count - static_cast<uint32_t>(result.size())));
        uint32_t cur_addr = address + static_cast<uint32_t>(result.size());

        if (verbose)
            std::println("  Reading {} bytes from ${:07X}", remaining, cur_addr);

        auto packet = protocol::build_mem_read(cur_addr, remaining, seq);

        if (transport.verbose) {
            std::println(stderr, "[read] -> sending {} byte routine to read {} bytes from ${:07X}",
                         packet.size(), remaining, cur_addr);
            std::print(stderr, "[read] -> hex: ");
            for (size_t i = 0; i < std::min(packet.size(), size_t{64}); i++)
                std::print(stderr, "{:02X} ", packet[i]);
            if (packet.size() > 64)
                std::print(stderr, "... ({} more)", packet.size() - 64);
            std::println(stderr, "");
        }

        bool got_response = false;
        for (int retry = 0; retry < DEFAULT_READ_RETRIES; retry++) {
            auto send_result = transport.send(packet);
            if (!send_result) {
                std::println(stderr, "etherdbg: send failed: {}",
                             to_string(send_result.error()));
                return {};
            }

            /* Try receiving multiple times within each retry window,
             * since beacon packets ("mega65") may arrive on the same
             * socket and need to be skipped. */
            for (int recv_attempt = 0; recv_attempt < 10; recv_attempt++) {
                auto recv_result = transport.recv(
                    protocol::RESPONSE_HEADER_SIZE + remaining + 256,
                    DEFAULT_RECV_TIMEOUT_MS);

                if (!recv_result) {
                    if (recv_result.error() == TransportError::Timeout) {
                        if (verbose)
                            std::println("  Timeout, retry {}/{}",
                                         retry + 1, DEFAULT_READ_RETRIES);
                        break;  /* go to next retry (re-send) */
                    }
                    std::println(stderr, "etherdbg: recv failed: {}",
                                 to_string(recv_result.error()));
                    return {};
                }

                auto& raw = *recv_result;
                if (transport.verbose) {
                    std::println(stderr, "[read] <- received {} bytes", raw.size());
                    std::print(stderr, "[read] <- hex: ");
                    for (size_t i = 0; i < std::min(raw.size(), size_t{64}); i++)
                        std::print(stderr, "{:02X} ", raw[i]);
                    if (raw.size() > 64)
                        std::print(stderr, "...");
                    std::println(stderr, "");
                }

                uint32_t resp_addr;
                uint8_t resp_seq;
                std::vector<uint8_t> resp_data;

                if (!protocol::parse_read_response(raw, resp_addr,
                                                    resp_seq, resp_data)) {
                    if (verbose)
                        std::println("  Skipping non-response packet ({} bytes, first byte ${:02X})",
                                     raw.size(), raw.empty() ? 0 : raw[0]);
                    continue;  /* try receiving again */
                }

                if (resp_seq != seq || resp_addr != cur_addr) {
                    if (verbose)
                        std::println("  Mismatched seq/addr (got seq={} addr=${:07X}), skipping",
                                     resp_seq, resp_addr);
                    continue;  /* try receiving again */
                }

                result.insert(result.end(), resp_data.begin(), resp_data.end());
                got_response = true;
                break;
            }

            if (got_response)
                break;
        }

        if (!got_response) {
            std::println(stderr,
                         "etherdbg: no response after {} retries at ${:07X}",
                         DEFAULT_READ_RETRIES, cur_addr);
            return {};
        }

        seq++;
    }

    if (verbose)
        std::println("Read {} bytes from ${:07X}", result.size(), address);

    return result;
}

int cmd_write_memory(Transport& transport,
                     uint32_t address, std::span<const uint8_t> data,
                     bool verbose)
{
    uint8_t seq = 0;
    size_t offset = 0;

    while (offset < data.size()) {
        auto chunk = static_cast<int>(
            std::min(static_cast<size_t>(protocol::MAX_CHUNK_SIZE),
                     data.size() - offset));
        uint32_t cur_addr = address + static_cast<uint32_t>(offset);

        if (verbose)
            std::println("  Writing {} bytes to ${:07X}", chunk, cur_addr);

        auto packet = protocol::build_mem_write(
            cur_addr, data.subspan(offset, chunk), seq);

        auto result = transport.send(packet);
        if (!result) {
            std::println(stderr, "etherdbg: send failed at offset {}: {}",
                         offset, to_string(result.error()));
            return -1;
        }

        std::this_thread::sleep_for(
            std::chrono::microseconds(DEFAULT_PACKET_DELAY_US));
        seq++;
        offset += chunk;
    }

    if (verbose)
        std::println("Wrote {} bytes to ${:07X}", data.size(), address);

    return 0;
}

int cmd_fill_memory(Transport& transport,
                    uint32_t address, uint32_t count, uint8_t value,
                    bool verbose)
{
    if (verbose)
        std::println("  Filling {} bytes at ${:07X} with ${:02X}",
                     count, address, value);

    uint8_t seq = 0;
    uint32_t offset = 0;

    while (offset < count) {
        auto chunk = static_cast<uint16_t>(
            std::min(count - offset, static_cast<uint32_t>(0xFFFF)));
        uint32_t cur_addr = address + offset;

        auto packet = protocol::build_mem_fill(cur_addr, chunk, value, seq);
        auto result = transport.send(packet);
        if (!result) {
            std::println(stderr, "etherdbg: send failed: {}",
                         to_string(result.error()));
            return -1;
        }

        std::this_thread::sleep_for(
            std::chrono::microseconds(DEFAULT_PACKET_DELAY_US));
        seq++;
        offset += chunk;
    }

    if (verbose)
        std::println("Filled {} bytes at ${:07X} with ${:02X}",
                     count, address, value);

    return 0;
}

} // namespace etherdbg
