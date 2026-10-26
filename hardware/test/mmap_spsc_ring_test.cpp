#include "../../exchange/mmap_spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

int main() {
    MmapSpscRing<std::uint64_t, 1024> ring;
    constexpr std::uint64_t count = 1'000'000;
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for (std::uint64_t value = 0; value < count; ++value) {
            while (!ring.try_push(value)) std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        for (std::uint64_t expected = 0; expected < count; ++expected) {
            std::uint64_t value = 0;
            while (!ring.try_pop(value)) std::this_thread::yield();
            if (value != expected) failed.store(true, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();
    if (failed.load(std::memory_order_relaxed)) {
        std::cerr << "mmap_spsc_ring_test: FAIL: ordering violation\n";
        return 1;
    }
    std::cout << "mmap_spsc_ring_test: PASS (" << count
              << " ordered records via " << ring.name() << ")\n";
    return 0;
}
