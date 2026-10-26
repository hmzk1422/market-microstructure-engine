#include "../../exchange/spsc_ring.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_full_and_empty() {
    SpscRing<std::uint32_t, 4> ring;
    std::uint32_t value = 0;
    require(!ring.try_pop(value), "empty ring must not return an item");
    require(ring.try_push(1) && ring.try_push(2) && ring.try_push(3),
            "ring should accept Capacity - 1 items");
    require(!ring.try_push(4), "full ring must reject instead of overwrite");
    for (std::uint32_t expected = 1; expected <= 3; ++expected) {
        require(ring.try_pop(value) && value == expected,
                "ring must preserve FIFO order");
    }
    require(!ring.try_pop(value), "ring should be empty after draining");
}

void test_concurrent_transfer() {
    constexpr std::uint64_t count = 1'000'000;
    SpscRing<std::uint64_t, 1024> ring;
    bool ordered = true;
    std::thread producer([&] {
        for (std::uint64_t value = 0; value < count; ++value) {
            while (!ring.try_push(value)) std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (expected < count) {
            std::uint64_t value = 0;
            if (!ring.try_pop(value)) {
                std::this_thread::yield();
                continue;
            }
            if (value != expected++) ordered = false;
        }
    });
    producer.join();
    consumer.join();
    require(ordered, "concurrent SPSC transfer must preserve every item");
}

} // namespace

int main() {
    try {
        test_full_and_empty();
        test_concurrent_transfer();
        std::cout << "spsc_ring_test: PASS (1,000,000 ordered items)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "spsc_ring_test: FAIL: " << error.what() << '\n';
        return 1;
    }
}
