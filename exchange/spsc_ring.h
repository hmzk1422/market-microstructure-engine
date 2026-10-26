#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

// Single-producer/single-consumer bounded ring. The producer never locks or
// waits; try_push returns false when the ring is full.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity > 1);

public:
    bool try_push(T value) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) % Capacity;
        if (next == read_.load(std::memory_order_acquire)) return false;
        slots_[write].emplace(std::move(value));
        write_.store(next, std::memory_order_release);
        wake_sequence_.fetch_add(1, std::memory_order_release);
        wake_sequence_.notify_one();
        return true;
    }

    bool try_pop(T& value) {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        value = std::move(*slots_[read]);
        slots_[read].reset();
        read_.store((read + 1) % Capacity, std::memory_order_release);
        return true;
    }

    void wait_for_data(std::size_t observed_sequence) const {
        wake_sequence_.wait(observed_sequence, std::memory_order_relaxed);
    }

    void wake() {
        wake_sequence_.fetch_add(1, std::memory_order_release);
        wake_sequence_.notify_all();
    }

    std::size_t wake_sequence() const {
        return wake_sequence_.load(std::memory_order_relaxed);
    }

private:
    // One slot remains empty to distinguish full from empty.
    std::array<std::optional<T>, Capacity> slots_{};
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
    mutable std::atomic<std::size_t> wake_sequence_{0};
};
