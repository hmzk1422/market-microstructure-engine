#pragma once

#include <chrono>
#include <array>
#include <cstdint>

enum class Venue {
    Bybit,
    Binance
};

struct QuoteUpdate {
    Venue venue;
    std::array<char, 32> symbol{};
    std::uint32_t bid;
    std::uint32_t ask;
    std::int64_t exchange_time_ms;
    std::chrono::steady_clock::time_point received_at;
};
