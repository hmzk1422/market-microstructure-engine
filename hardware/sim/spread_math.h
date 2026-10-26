#pragma once

#include <cstdint>

namespace strategy {

struct NetSpread {
    std::int64_t value = 0;
    std::uint8_t direction = 0;
};

inline NetSpread calculate_net_spread(std::uint32_t bybit_bid,
                                      std::uint32_t bybit_ask,
                                      std::uint32_t binance_bid,
                                      std::uint32_t binance_ask,
                                      std::uint16_t bybit_fee_bps,
                                      std::uint16_t binance_fee_bps) {
    constexpr std::int64_t scale = 10'000;
    const std::int64_t buy_bybit =
        static_cast<std::int64_t>(binance_bid) *
            (scale - binance_fee_bps) -
        static_cast<std::int64_t>(bybit_ask) *
            (scale + bybit_fee_bps);
    const std::int64_t buy_binance =
        static_cast<std::int64_t>(bybit_bid) *
            (scale - bybit_fee_bps) -
        static_cast<std::int64_t>(binance_ask) *
            (scale + binance_fee_bps);
    return buy_bybit >= buy_binance
               ? NetSpread{buy_bybit, 1}
               : NetSpread{buy_binance, 2};
}

} // namespace strategy
