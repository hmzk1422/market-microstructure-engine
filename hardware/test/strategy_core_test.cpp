#include "strategy_core.h"
#include "spread_math.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_warmup_and_risk_gate() {
    strategy::RollingZScore window;
    for (int i = 1; i <= 50; ++i) window.push(static_cast<double>(i));
    require(window.ready(), "window should be ready after 50 observations");
    const auto score = window.score(100.0);
    require(score > 2.0, "large positive spread should exceed +2 sigma");
    require(strategy::evaluate_risk(true, false, score, 2.0).reason ==
                std::string("ZSCORE_WARMUP"),
            "candidate must wait for a complete history window");
    require(strategy::evaluate_risk(true, true, score, 2.0).accepted,
            "candidate with a ready, strong positive z-score should pass");
    require(!strategy::evaluate_risk(false, true, score, 2.0).accepted,
            "C++ must never accept an RTL rejection");
    require(!strategy::evaluate_risk(true, true, 0.5, 2.0).accepted,
            "candidate below threshold should be rejected");
}

void test_constant_history_and_rolling_eviction() {
    strategy::RollingZScore constant;
    for (int i = 0; i < 50; ++i) constant.push(3.0);
    require(constant.score(3.0) == 0.0,
            "zero variance must produce a finite zero z-score");
    require(constant.score(4.0) == 0.0,
            "zero variance must not generate an infinite signal");

    strategy::RollingZScore rolling;
    for (int i = 1; i <= 50; ++i) rolling.push(static_cast<double>(i));
    rolling.push(51.0);
    require(rolling.size() == strategy::RollingZScore::capacity,
            "rolling window must remain bounded after eviction");
    require(rolling.score(60.0) > 2.0,
            "evicted observations must not corrupt the running statistics");
}

void test_512_independent_symbols() {
    constexpr std::size_t symbols = 512;
    std::vector<strategy::RollingZScore> windows(symbols);
    for (std::size_t sample = 0; sample < 75; ++sample) {
        for (std::size_t symbol = 0; symbol < symbols; ++symbol) {
            const double baseline = 1.0 + static_cast<double>(symbol) / 100.0;
            const double noise = static_cast<double>((sample * 17 + symbol * 7) % 13) / 1000.0;
            windows[symbol].push(baseline + noise);
        }
    }
    for (const auto& window : windows) {
        require(window.ready(), "each symbol needs its own warmed history");
        require(std::isfinite(window.score(2.0)),
                "all per-symbol z-scores must stay finite");
    }
}

} // namespace

int main() {
    const auto spread_a = strategy::calculate_net_spread(
        1'000'000, 1'001'000, 1'002'000, 1'003'000, 0, 0);
    require(spread_a.value == 10'000'000 && spread_a.direction == 1,
            "C++ raw-price spread matches the RTL fixed-point formula");
    const auto spread_b = strategy::calculate_net_spread(
        1'002'000, 1'003'000, 1'000'000, 1'001'000, 0, 0);
    require(spread_b.value == 10'000'000 && spread_b.direction == 2,
            "C++ independently selects the opposite arbitrage direction");

    try {
        test_warmup_and_risk_gate();
        test_constant_history_and_rolling_eviction();
        test_512_independent_symbols();
        std::cout << "strategy_core_test: PASS (512 symbols, 50-observation rolling window)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strategy_core_test: FAIL: " << error.what() << '\n';
        return 1;
    }
}
