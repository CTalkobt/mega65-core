/* Test: discover + create transport, but don't send anything */
#include <print>
#include <thread>
#include <chrono>
#include "transport_udp.h"

int main() {
    std::println("Step 1: Discovering MEGA65...");
    auto addr = etherdbg::discover_mega65(3000, true);
    if (addr.empty()) {
        std::println("Not found.");
        return 1;
    }
    std::println("Found: {}", addr);

    std::println("Step 2: Creating transport socket (no sends)...");
    auto transport = etherdbg::create_udp_transport(addr);
    if (!transport) {
        std::println("Failed to create transport.");
        return 1;
    }
    std::println("Transport created. Waiting 3 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::println("Step 3: Did the screen stay clean after transport creation?");
    std::println("        Press Enter, then I'll send hyperrupt...");
    getchar();

    std::println("Step 4: Sending hyperrupt...");
    transport->activate();
    std::println("Hyperrupt sent. Waiting 3 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::println("Step 5: Did the screen get garbage after hyperrupt?");
    return 0;
}
