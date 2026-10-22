#include "Vspread_detector.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint64_t kFeeScale = 10'000;
constexpr std::uint32_t kBuyBybitSellBinance = 1;
constexpr std::uint32_t kBuyBinanceSellBybit = 2;

struct QuotePair {
    std::uint32_t bybit_bid;
    std::uint32_t bybit_ask;
    std::uint32_t binance_bid;
    std::uint32_t binance_ask;
    std::uint16_t bybit_fee_bps;
    std::uint16_t binance_fee_bps;
    std::uint64_t min_profit;
    std::uint16_t symbol_id;
    std::uint32_t transaction_id;
    bool input_valid = true;
    bool quotes_valid = true;
};

struct Expected {
    std::int64_t net_profit;
    std::uint32_t direction;
    bool candidate;
};

struct Simulation {
    Vspread_detector dut;
    VerilatedVcdC trace;
    std::uint64_t time = 0;
    std::uint64_t cycle = 0;

    explicit Simulation(const char* trace_path) {
        Verilated::traceEverOn(true);
        dut.trace(&trace, 99);
        trace.open(trace_path);
        dut.clk = 0;
        dut.rst_n = 0;
        dut.input_valid = 0;
        dut.quotes_valid = 0;
        dut.bybit_bid_i = 0;
        dut.bybit_ask_i = 0;
        dut.binance_bid_i = 0;
        dut.binance_ask_i = 0;
        dut.bybit_fee_bps_i = 0;
        dut.binance_fee_bps_i = 0;
        dut.min_profit_i = 0;
        dut.symbol_id_i = 0;
        dut.transaction_id_i = 0;
        dut.eval();
        dump();
    }

    ~Simulation() {
        dut.final();
        trace.close();
    }

    void dump() {
        trace.dump(time++);
    }

    std::int64_t signed_net_profit() const {
        constexpr std::uint64_t mask = (std::uint64_t{1} << 49) - 1;
        const std::uint64_t raw = static_cast<std::uint64_t>(dut.net_profit_o) & mask;
        return static_cast<std::int64_t>((raw ^ (std::uint64_t{1} << 48)) -
                                         (std::uint64_t{1} << 48));
    }

    void reset() {
        dut.rst_n = 0;
        dut.input_valid = 0;
        dut.quotes_valid = 0;
        dut.clk = 0;
        dut.eval();
        dump();
        dut.clk = 1;
        dut.eval();
        dump();
        dut.clk = 0;
        dut.eval();
        dump();
        if (dut.candidate_valid_o || dut.direction_o || dut.net_profit_o ||
            dut.symbol_id_o || dut.transaction_id_o) {
            throw std::runtime_error("reset did not clear outputs");
        }
        dut.rst_n = 1;
        dut.eval();
        dump();
        std::cout << "[TEST] reset cleared registered outputs\n";
    }

    Expected reference(const QuotePair& q) const {
        if (!q.input_valid || !q.quotes_valid ||
            q.bybit_bid == 0 || q.bybit_ask == 0 ||
            q.binance_bid == 0 || q.binance_ask == 0 ||
            q.bybit_fee_bps > kFeeScale ||
            q.binance_fee_bps > kFeeScale) {
            return {0, 0, false};
        }

        const auto proceeds = [](std::uint32_t bid, std::uint16_t fee) {
            return static_cast<std::uint64_t>(bid) * (kFeeScale - fee);
        };
        const auto cost = [](std::uint32_t ask, std::uint16_t fee) {
            return static_cast<std::uint64_t>(ask) * (kFeeScale + fee);
        };

        const std::int64_t buy_bybit =
            static_cast<std::int64_t>(proceeds(q.binance_bid, q.binance_fee_bps)) -
            static_cast<std::int64_t>(cost(q.bybit_ask, q.bybit_fee_bps));
        const std::int64_t buy_binance =
            static_cast<std::int64_t>(proceeds(q.bybit_bid, q.bybit_fee_bps)) -
            static_cast<std::int64_t>(cost(q.binance_ask, q.binance_fee_bps));
        const bool first_is_best = buy_bybit >= buy_binance;
        const std::int64_t best = first_is_best ? buy_bybit : buy_binance;
        const std::uint32_t direction =
            first_is_best ? kBuyBybitSellBinance : kBuyBinanceSellBybit;
        return {best, direction,
                best > static_cast<std::int64_t>(q.min_profit)};
    }

    void drive(const QuotePair& q, const std::string& label, bool print = true) {
        const Expected expected = reference(q);
        const bool old_candidate = dut.candidate_valid_o != 0;
        const std::int64_t old_net = signed_net_profit();
        const auto old_direction = dut.direction_o;
        const auto old_symbol_id = dut.symbol_id_o;
        const auto old_transaction_id = dut.transaction_id_o;
        const auto old_valid = dut.candidate_valid_o;
        dut.clk = 0;
        dut.input_valid = q.input_valid;
        dut.quotes_valid = q.quotes_valid;
        dut.bybit_bid_i = q.bybit_bid;
        dut.bybit_ask_i = q.bybit_ask;
        dut.binance_bid_i = q.binance_bid;
        dut.binance_ask_i = q.binance_ask;
        dut.bybit_fee_bps_i = q.bybit_fee_bps;
        dut.binance_fee_bps_i = q.binance_fee_bps;
        dut.min_profit_i = q.min_profit;
        dut.symbol_id_i = q.symbol_id;
        dut.transaction_id_i = q.transaction_id;
        dut.eval();
        dump();

        if (signed_net_profit() != old_net ||
            dut.direction_o != old_direction ||
            dut.symbol_id_o != old_symbol_id ||
            dut.transaction_id_o != old_transaction_id ||
            dut.candidate_valid_o != old_valid) {
            throw std::runtime_error("outputs changed before the active clock edge");
        }
        dut.clk = 1;
        dut.eval();
        dump();
        ++cycle;

        const std::int64_t actual_net = signed_net_profit();
        if ((dut.candidate_valid_o != 0) != expected.candidate ||
            actual_net != expected.net_profit ||
            dut.direction_o != (expected.candidate ? expected.direction : 0) ||
            dut.symbol_id_o != (q.input_valid && q.quotes_valid &&
                                q.bybit_bid && q.bybit_ask &&
                                q.binance_bid && q.binance_ask &&
                                q.bybit_fee_bps <= kFeeScale &&
                                q.binance_fee_bps <= kFeeScale
                                    ? q.symbol_id : 0) ||
            dut.transaction_id_o != (q.input_valid && q.quotes_valid &&
                                     q.bybit_bid && q.bybit_ask &&
                                     q.binance_bid && q.binance_ask &&
                                     q.bybit_fee_bps <= kFeeScale &&
                                     q.binance_fee_bps <= kFeeScale
                                         ? q.transaction_id : 0)) {
            throw std::runtime_error("RTL/reference mismatch at " + label);
        }
        if (old_candidate && !q.input_valid) {
            throw std::runtime_error("candidate_valid did not clear on invalid cycle");
        }

        if (print) {
            std::cout << "[RTL] cycle=" << cycle
                      << " label=" << label
                      << " input_valid=" << static_cast<unsigned>(q.input_valid)
                      << " quotes_valid=" << static_cast<unsigned>(q.quotes_valid)
                      << " symbol_id=" << q.symbol_id
                      << " transaction_id=" << q.transaction_id
                      << " bybit_bid=" << q.bybit_bid
                      << " bybit_ask=" << q.bybit_ask
                      << " binance_bid=" << q.binance_bid
                      << " binance_ask=" << q.binance_ask
                      << " candidate_valid="
                      << static_cast<unsigned>(dut.candidate_valid_o)
                      << " direction=" << static_cast<unsigned>(dut.direction_o)
                      << " net_profit=" << actual_net << '\n';
        }
        dut.clk = 0;
        dut.eval();
        dump();
    }
};

QuotePair defaults() {
    return {100, 101, 102, 103, 0, 0, 0, 7, 1};
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* trace_path =
        argc > 1 ? argv[1] : "spread_detector.vcd";
    try {
        Simulation sim(trace_path);
        require(sim.dut.input_ready == 1, "input_ready must remain asserted");
        sim.reset();

        QuotePair q = defaults();
        sim.drive(q, "buy-bybit-sell-binance");
        require(sim.dut.candidate_valid_o == 1 &&
                    sim.dut.direction_o == kBuyBybitSellBinance,
                "expected BUY_BYBIT_SELL_BINANCE candidate");
        sim.reset();

        q = {102, 103, 100, 101, 0, 0, 0, 8, 2};
        sim.drive(q, "buy-binance-sell-bybit");
        require(sim.dut.candidate_valid_o == 1 &&
                    sim.dut.direction_o == kBuyBinanceSellBybit,
                "expected BUY_BINANCE_SELL_BYBIT candidate");

        q = defaults();
        q.min_profit = 10'000;
        sim.drive(q, "threshold-equality-rejected");
        require(sim.dut.candidate_valid_o == 0,
                "profit equal to threshold must be rejected");
        q.min_profit = 9'999;
        q.transaction_id = 3;
        sim.drive(q, "threshold-one-unit-below");
        require(sim.dut.candidate_valid_o == 1,
                "profit above threshold must be accepted");

        q = defaults();
        q.bybit_fee_bps = 10'000;
        q.binance_fee_bps = 10'000;
        q.min_profit = 0;
        q.transaction_id = 4;
        sim.drive(q, "fee-adjusted-nonprofit");
        require(sim.dut.candidate_valid_o == 0,
                "fees must be included in profitability");

        q = defaults();
        q.binance_bid = std::numeric_limits<std::uint32_t>::max();
        q.bybit_ask = 1;
        q.bybit_bid = 1;
        q.binance_ask = std::numeric_limits<std::uint32_t>::max();
        q.transaction_id = 5;
        sim.drive(q, "maximum-price-arithmetic");

        q = defaults();
        q.transaction_id = 6;
        q.quotes_valid = false;
        sim.drive(q, "incomplete-quotes");
        q = defaults();
        q.transaction_id = 7;
        q.bybit_fee_bps = 10'001;
        sim.drive(q, "out-of-range-fee");
        q = defaults();
        q.transaction_id = 8;
        q.binance_ask = 0;
        sim.drive(q, "zero-price-rejected");

        q = defaults();
        q.transaction_id = 9;
        sim.drive(q, "consecutive-transaction-1");
        q.transaction_id = 10;
        q.bybit_ask = 200;
        sim.drive(q, "consecutive-transaction-2");
        require(sim.dut.candidate_valid_o == 0,
                "non-candidate must clear previous valid output");

        constexpr std::uint32_t symbol_count = 512;
        constexpr std::uint32_t iterations = 4;
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t candidates = 0;
        for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
            for (std::uint32_t symbol = 0; symbol < symbol_count; ++symbol) {
                q = defaults();
                q.symbol_id = static_cast<std::uint16_t>(symbol);
                q.transaction_id =
                    iteration * symbol_count + symbol + 11;
                q.bybit_ask = 101 + (symbol % 3);
                q.binance_bid = 102 + (symbol % 5);
                sim.drive(q, "synthetic-load", false);
                candidates += sim.dut.candidate_valid_o != 0;
            }
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        require(candidates > 0, "synthetic load produced no candidates");
        std::cout << "[LOAD] symbols=" << symbol_count
                  << " transactions=" << symbol_count * iterations
                  << " candidates=" << candidates
                  << " simulated_transactions_per_second="
                  << (symbol_count * iterations / elapsed) << '\n';
        std::cout << "[TEST] all RTL/reference checks passed cycles=" << sim.cycle
                  << " waveform=" << trace_path << '\n';
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
