/*
 * test_etherdbg.cpp - Unit tests for etherdbg
 *
 * Tests protocol packet construction, mock transport, and command logic.
 * Self-contained -- no external test framework required.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <vector>

#include "protocol.h"
#include "transport.h"
#include "commands.h"

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(); \
    static struct Register_##name { \
        Register_##name() { register_test(#name, test_##name); } \
    } reg_##name; \
    static void test_##name()

struct TestEntry { const char* name; void (*fn)(); };
static std::vector<TestEntry>& test_registry() {
    static std::vector<TestEntry> tests;
    return tests;
}
static void register_test(const char* name, void (*fn)()) {
    test_registry().push_back({name, fn});
}

static bool current_test_failed = false;

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::println(stderr, "  FAIL: {}:{}: {}", __FILE__, __LINE__, #expr); \
        current_test_failed = true; return; \
    } \
} while (0)

template<typename T>
auto to_printable(T val) {
    if constexpr (std::is_enum_v<T>)
        return static_cast<std::underlying_type_t<T>>(val);
    else
        return val;
}

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        std::println(stderr, "  FAIL: {}:{}: {} == {} (got {} vs {})", \
                     __FILE__, __LINE__, #a, #b, \
                     to_printable(_a), to_printable(_b)); \
        current_test_failed = true; return; \
    } \
} while (0)

// ---------------------------------------------------------------------------
// Mock transport for command-layer tests
// ---------------------------------------------------------------------------

class MockTransport final : public etherdbg::Transport {
public:
    std::vector<std::vector<uint8_t>> sent_packets;
    bool fail_sends = false;

    std::expected<size_t, etherdbg::TransportError>
    send(std::span<const uint8_t> data) override {
        if (fail_sends)
            return std::unexpected(etherdbg::TransportError::SendFailed);
        sent_packets.emplace_back(data.begin(), data.end());
        return data.size();
    }

    std::expected<std::vector<uint8_t>, etherdbg::TransportError>
    recv(size_t /*max_len*/, int /*timeout_ms*/) override {
        return std::unexpected(etherdbg::TransportError::Timeout);
    }
};

// ---------------------------------------------------------------------------
// Helper: write a temporary PRG file with given load address and data
// ---------------------------------------------------------------------------

class TempPrg {
public:
    TempPrg(uint16_t load_addr, std::span<const uint8_t> data) {
        path_ = std::filesystem::temp_directory_path() / "etherdbg_test.prg";
        std::ofstream f(path_, std::ios::binary);
        uint8_t hdr[2] = {
            static_cast<uint8_t>(load_addr & 0xff),
            static_cast<uint8_t>((load_addr >> 8) & 0xff)
        };
        f.write(reinterpret_cast<char*>(hdr), 2);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    ~TempPrg() { std::filesystem::remove(path_); }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

// ===========================================================================
// Protocol tests
// ===========================================================================

/* Original byte arrays from etherload.c for reference */
static constexpr uint8_t original_dma[] = {
    0xa9, 0xff, 0x8d, 0x05, 0xd7, 0xad, 0x68, 0x68,
    0x8d, 0x06, 0xd7, 0xa9, 0x0d, 0x8d, 0x02, 0xd7,
    0xa9, 0xe8, 0x8d, 0x01, 0xd7, 0xa9, 0xff, 0x8d,
    0x04, 0xd7, 0xa9, 0x5c, 0x8d, 0x00, 0xd7, 0xae,
    0x67, 0x68, 0xea, 0x9d, 0x80, 0x06, 0xee, 0x26,
    0x04, 0xd0, 0x03, 0xee, 0x25, 0x04, 0x60, 0x00,
    0x00, 0x00, 0x04, 0x80, 0xe8, 0x8d, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00
};

static constexpr uint8_t original_done[] = {
    0xa9, 0x00, 0xee, 0x20, 0xd0, 0x4c, 0x2c, 0x68,
    0xa9, 0x00, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea,
    0xa2, 0x00, 0xbd, 0x44, 0x68, 0x9d, 0x40, 0x03,
    0xe8, 0xe0, 0x40, 0xd0, 0xf5, 0x4c, 0x40, 0x03,
    0xa9, 0x47, 0x8d, 0x2f, 0xd0, 0xa9, 0x53, 0x8d,
    0x2f, 0xd0, 0xa9, 0x00, 0xa2, 0x0f, 0xa0, 0x00,
    0xa3, 0x00, 0x5c, 0xea, 0xa9, 0x00, 0xa2, 0x00,
    0xa0, 0x00, 0xa3, 0x00, 0x5c, 0xea, 0x68, 0x68,
    0x60
};

TEST(protocol_dma_packet_size) {
    auto pkt = etherdbg::protocol::build_dma_load(0x0800, 0, 0, {}, 0);
    ASSERT_EQ(static_cast<int>(pkt.size()),
              etherdbg::protocol::DMA_PACKET_SIZE);
}

TEST(protocol_done_packet_size) {
    auto pkt = etherdbg::protocol::build_done();
    ASSERT_EQ(static_cast<int>(pkt.size()),
              etherdbg::protocol::DONE_PACKET_SIZE);
}

TEST(protocol_dma_starts_with_lda) {
    auto pkt = etherdbg::protocol::build_dma_load(0x0800, 0, 0, {}, 0);
    ASSERT_EQ(pkt[0], 0xa9);  // LDA #imm -- required by ETHLOAD listener
}

TEST(protocol_done_starts_with_lda) {
    auto pkt = etherdbg::protocol::build_done();
    ASSERT_EQ(pkt[0], 0xa9);
}

TEST(protocol_dma_routine_matches_original) {
    // Build with same defaults as original etherload.c template
    auto pkt = etherdbg::protocol::build_dma_load(0x1000, 0, 0, {}, 0x30);
    // Original had byte count 0x0400 baked in; ours patches to 0 for empty data
    pkt[0x31] = 0x00;
    pkt[0x32] = 0x04;
    ASSERT_TRUE(std::memcmp(pkt.data(), original_dma, sizeof(original_dma)) == 0);
}

TEST(protocol_done_matches_original) {
    auto pkt = etherdbg::protocol::build_done();
    ASSERT_TRUE(std::memcmp(pkt.data(), original_done, sizeof(original_done)) == 0);
}

TEST(protocol_dma_patches_address) {
    auto pkt = etherdbg::protocol::build_dma_load(0xABCD, 0, 0, {}, 0);
    ASSERT_EQ(pkt[0x36], 0xCD);  // low byte
    ASSERT_EQ(pkt[0x37], 0xAB);  // high byte
}

TEST(protocol_dma_patches_bank_and_mb) {
    auto pkt = etherdbg::protocol::build_dma_load(0x0000, 0x12, 0x34, {}, 0);
    ASSERT_EQ(pkt[0x38], 0x12);  // bank
    ASSERT_EQ(pkt[0x3c], 0x34);  // megabyte
}

TEST(protocol_dma_patches_seq) {
    auto pkt = etherdbg::protocol::build_dma_load(0x0800, 0, 0, {}, 0x42);
    ASSERT_EQ(pkt[0x3b], 0x42);
}

TEST(protocol_dma_patches_byte_count) {
    std::vector<uint8_t> data(256, 0xAA);
    auto pkt = etherdbg::protocol::build_dma_load(0x0800, 0, 0, data, 0);
    ASSERT_EQ(pkt[0x31], 0x00);  // 256 & 0xff
    ASSERT_EQ(pkt[0x32], 0x01);  // 256 >> 8
}

TEST(protocol_dma_copies_data) {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    auto pkt = etherdbg::protocol::build_dma_load(0x0800, 0, 0, data, 0);
    constexpr int data_offset = 0x80 - 0x2c;
    ASSERT_EQ(pkt[data_offset + 0], 0xDE);
    ASSERT_EQ(pkt[data_offset + 1], 0xAD);
    ASSERT_EQ(pkt[data_offset + 2], 0xBE);
    ASSERT_EQ(pkt[data_offset + 3], 0xEF);
}

TEST(protocol_dma_max_chunk) {
    std::vector<uint8_t> data(etherdbg::protocol::MAX_CHUNK_SIZE, 0x55);
    auto pkt = etherdbg::protocol::build_dma_load(0x0800, 0, 0, data, 0);
    // Should not crash or overflow
    ASSERT_EQ(static_cast<int>(pkt.size()),
              etherdbg::protocol::DMA_PACKET_SIZE);
    constexpr int data_offset = 0x80 - 0x2c;
    ASSERT_EQ(pkt[data_offset], 0x55);
    ASSERT_EQ(pkt[data_offset + etherdbg::protocol::MAX_CHUNK_SIZE - 1], 0x55);
}

TEST(protocol_done_remainder_is_zero) {
    auto pkt = etherdbg::protocol::build_done();
    // Bytes after the template should be zero-filled
    for (size_t i = sizeof(original_done); i < pkt.size(); i++)
        ASSERT_EQ(pkt[i], 0);
}

// ===========================================================================
// Command tests (using MockTransport)
// ===========================================================================

TEST(cmd_load_small_prg) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
    TempPrg prg(0x0801, data);

    MockTransport mock;
    int ret = etherdbg::cmd_load_program(mock, prg.path().string(), false);
    ASSERT_EQ(ret, 0);

    // 1 data packet + DONE_REPEAT_COUNT done packets
    ASSERT_EQ(static_cast<int>(mock.sent_packets.size()),
              1 + etherdbg::DONE_REPEAT_COUNT);

    // Data packet should have correct size
    ASSERT_EQ(static_cast<int>(mock.sent_packets[0].size()),
              etherdbg::protocol::DMA_PACKET_SIZE);

    // Done packets should have correct size
    ASSERT_EQ(static_cast<int>(mock.sent_packets[1].size()),
              etherdbg::protocol::DONE_PACKET_SIZE);
}

TEST(cmd_load_checks_address) {
    std::vector<uint8_t> data = {0xAA, 0xBB};
    TempPrg prg(0x2000, data);

    MockTransport mock;
    etherdbg::cmd_load_program(mock, prg.path().string(), false);

    // Data packet should target address $2000
    auto& pkt = mock.sent_packets[0];
    ASSERT_EQ(pkt[0x36], 0x00);  // low byte of $2000
    ASSERT_EQ(pkt[0x37], 0x20);  // high byte
}

TEST(cmd_load_multi_chunk) {
    // Create data larger than one chunk to force multiple packets
    std::vector<uint8_t> data(etherdbg::protocol::MAX_CHUNK_SIZE + 100, 0x42);
    TempPrg prg(0x0800, data);

    MockTransport mock;
    int ret = etherdbg::cmd_load_program(mock, prg.path().string(), false);
    ASSERT_EQ(ret, 0);

    // Should have 2 data packets + done packets
    ASSERT_EQ(static_cast<int>(mock.sent_packets.size()),
              2 + etherdbg::DONE_REPEAT_COUNT);

    // Second data packet should target address $0800 + MAX_CHUNK_SIZE
    auto& pkt2 = mock.sent_packets[1];
    uint16_t expected_addr = 0x0800 + etherdbg::protocol::MAX_CHUNK_SIZE;
    ASSERT_EQ(pkt2[0x36], static_cast<uint8_t>(expected_addr & 0xff));
    ASSERT_EQ(pkt2[0x37], static_cast<uint8_t>((expected_addr >> 8) & 0xff));
}

TEST(cmd_load_seq_increments) {
    std::vector<uint8_t> data(etherdbg::protocol::MAX_CHUNK_SIZE * 3, 0x00);
    TempPrg prg(0x0800, data);

    MockTransport mock;
    etherdbg::cmd_load_program(mock, prg.path().string(), false);

    ASSERT_EQ(mock.sent_packets[0][0x3b], 0x00);  // seq 0
    ASSERT_EQ(mock.sent_packets[1][0x3b], 0x01);  // seq 1
    ASSERT_EQ(mock.sent_packets[2][0x3b], 0x02);  // seq 2
}

TEST(cmd_load_nonexistent_file) {
    MockTransport mock;
    int ret = etherdbg::cmd_load_program(mock, "/no/such/file.prg", false);
    ASSERT_EQ(ret, -1);
    ASSERT_TRUE(mock.sent_packets.empty());
}

TEST(cmd_load_truncated_file) {
    // File with only 1 byte -- can't read 2-byte load address
    auto path = std::filesystem::temp_directory_path() / "etherdbg_trunc.prg";
    {
        std::ofstream f(path, std::ios::binary);
        f.put(0x01);
    }
    MockTransport mock;
    int ret = etherdbg::cmd_load_program(mock, path.string(), false);
    ASSERT_EQ(ret, -1);
    std::filesystem::remove(path);
}

TEST(cmd_load_send_failure) {
    std::vector<uint8_t> data = {0x01, 0x02};
    TempPrg prg(0x0800, data);

    MockTransport mock;
    mock.fail_sends = true;
    int ret = etherdbg::cmd_load_program(mock, prg.path().string(), false);
    ASSERT_EQ(ret, -1);
}

TEST(cmd_load_done_packets_match) {
    std::vector<uint8_t> data = {0x01};
    TempPrg prg(0x0800, data);

    MockTransport mock;
    etherdbg::cmd_load_program(mock, prg.path().string(), false);

    // All done packets should be identical
    auto ref = etherdbg::protocol::build_done();
    for (int i = 1; i <= etherdbg::DONE_REPEAT_COUNT; i++) {
        ASSERT_EQ(mock.sent_packets[i].size(), ref.size());
        ASSERT_TRUE(mock.sent_packets[i] == ref);
    }
}

TEST(cmd_load_data_integrity) {
    // Verify actual payload data arrives in the packet
    std::vector<uint8_t> data = {0xCA, 0xFE, 0xBA, 0xBE};
    TempPrg prg(0x0800, data);

    MockTransport mock;
    etherdbg::cmd_load_program(mock, prg.path().string(), false);

    constexpr int data_offset = 0x80 - 0x2c;
    auto& pkt = mock.sent_packets[0];
    ASSERT_EQ(pkt[data_offset + 0], 0xCA);
    ASSERT_EQ(pkt[data_offset + 1], 0xFE);
    ASSERT_EQ(pkt[data_offset + 2], 0xBA);
    ASSERT_EQ(pkt[data_offset + 3], 0xBE);
}

// ===========================================================================
// Transport interface tests
// ===========================================================================

TEST(mock_transport_records_sends) {
    MockTransport mock;
    std::vector<uint8_t> data = {1, 2, 3};
    auto result = mock.send(data);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value(), size_t{3});
    ASSERT_EQ(mock.sent_packets.size(), size_t{1});
    ASSERT_TRUE(mock.sent_packets[0] == data);
}

TEST(mock_transport_fail_mode) {
    MockTransport mock;
    mock.fail_sends = true;
    auto result = mock.send(std::vector<uint8_t>{1});
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error(), etherdbg::TransportError::SendFailed);
}

TEST(mock_transport_recv_returns_timeout) {
    MockTransport mock;
    auto result = mock.recv(1024, 0);
    ASSERT_TRUE(!result.has_value());
    ASSERT_EQ(result.error(), etherdbg::TransportError::Timeout);
}

TEST(transport_error_to_string) {
    ASSERT_TRUE(etherdbg::to_string(etherdbg::TransportError::InitFailed)
                == "initialization failed");
    ASSERT_TRUE(etherdbg::to_string(etherdbg::TransportError::SendFailed)
                == "send failed");
    ASSERT_TRUE(etherdbg::to_string(etherdbg::TransportError::Timeout)
                == "timeout");
}

// ===========================================================================
// Main
// ===========================================================================

int main()
{
    for (auto& [name, fn] : test_registry()) {
        current_test_failed = false;
        tests_run++;
        fn();
        if (current_test_failed) {
            tests_failed++;
            std::println("  FAIL: {}", name);
        } else {
            tests_passed++;
            std::println("  PASS: {}", name);
        }
    }

    std::println("\n{} tests: {} passed, {} failed",
                 tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
