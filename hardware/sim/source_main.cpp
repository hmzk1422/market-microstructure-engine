#include "Vspread_detector.h"
#include "verilated.h"

#include "../../exchange/config.h"
#include "../../exchange/market_data.h"
#include "../../exchange/socket_reader.h"
#include "../../exchange/websocket_feed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <ios>
#include <streambuf>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sstream>
#include <iostream>
#include <map>

#ifdef _WIN32
#include <csignal>
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

constexpr std::uint64_t kPriceScale = 10'000;
constexpr std::uint64_t kFeeScale = 10'000;
constexpr std::size_t kDecisionQueueCapacity = 4096;
constexpr std::size_t kMaximumConfiguredSymbols = 65'535;

class TeeStreamBuffer final : public std::streambuf {
public:
    TeeStreamBuffer(std::streambuf* console, std::streambuf* file)
        : console_(console), file_(file) {}

    std::streambuf* console_buffer() const { return console_; }

protected:
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }

        const auto value = traits_type::to_char_type(character);
        if (traits_type::eq_int_type(console_->sputc(value),
                                     traits_type::eof()) ||
            traits_type::eq_int_type(file_->sputc(value),
                                     traits_type::eof())) {
            return traits_type::eof();
        }
        return character;
    }

    int sync() override {
        const int console_result = console_->pubsync();
        const int file_result = file_->pubsync();
        return console_result == 0 && file_result == 0 ? 0 : -1;
    }

private:
    std::streambuf* console_;
    std::streambuf* file_;
};

struct BenchmarkReport {
    std::uint64_t total_updates = 0;
    std::uint64_t total_candidates = 0;
    std::uint64_t stale_quotes = 0;
    std::uint64_t malformed_packets = 0;
    std::uint64_t decision_queue_dropped = 0;
    std::uint64_t websocket_dropped = 0;
    std::uint64_t websocket_malformed = 0;
    std::double_t runtime_seconds = 0.0;
    std::double_t updates_per_second = 0.0;
    std::double_t candidate_rate = 0.0;
    std::uint64_t total_updates_last = 0;
    std::uint64_t total_candidates_last = 0;

    std::string format() const {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2);
        os << "benchmarks updates=" << total_updates
           << " candidates=" << total_candidates
           << " stale_quotes=" << stale_quotes
           << " malformed=" << malformed_packets
           << " dropped=" << decision_queue_dropped;
        if (updates_per_second > 0.0) {
            os << " rate=" << updates_per_second << "updates/s";
        }
        if (candidate_rate > 0.0) {
            os << " candidate_rate=" << (candidate_rate * 100.0) << "%";
        }
        return os.str();
    }
};

class LatencySamples {
public:
    void add(std::chrono::steady_clock::duration duration) {
        const auto nanoseconds = std::chrono::duration_cast<
            std::chrono::nanoseconds>(duration).count();
        std::lock_guard<std::mutex> lock(mutex_);
        ++observed_;
        if (samples_.size() < kMaximumSamples) {
            samples_.push_back(static_cast<std::uint64_t>(nanoseconds));
        }
    }

    std::string summary() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty()) {
            return "no samples";
        }
        auto sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&sorted](double fraction) {
            const auto index = static_cast<std::size_t>(
                fraction * static_cast<double>(sorted.size() - 1));
            return sorted[index];
        };
        const auto total = std::accumulate(
            sorted.begin(), sorted.end(), std::uint64_t{0});
        const double mean = static_cast<double>(total) / sorted.size();
        std::ostringstream output;
        output << "observed=" << observed_
               << " retained=" << sorted.size() << std::fixed
               << std::setprecision(0) << " mean_ns=" << mean
               << " p50_ns=" << percentile(0.50)
               << " p95_ns=" << percentile(0.95)
               << " p99_ns=" << percentile(0.99)
               << " max_ns=" << sorted.back();
        return output.str();
    }

private:
    static constexpr std::size_t kMaximumSamples = 250'000;
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> samples_;
    std::uint64_t observed_ = 0;
};

struct RuntimeMetrics {
    LatencySamples websocket_callback_to_rtl_output;
    LatencySamples rtl_eval_wall_time;
    LatencySamples websocket_callback_to_strategy_enqueue;
    LatencySamples strategy_queue_wait;
};

std::atomic<bool> running{true};

void signal_handler(int) {
    running.store(false, std::memory_order_relaxed);
}

struct Options {
    std::string source = "abides";
    std::string socket = Config{}.socket_path;
    std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT"};
    // The websocket executable is a paper-trading demo. Set account-specific
    // fees explicitly when evaluating whether a gross spread is executable.
    std::uint16_t bybit_fee_bps = 0;
    std::uint16_t binance_fee_bps = 0;
    std::uint64_t min_profit = 0;
    std::uint32_t max_quote_age_ms = 1'500;
    bool verbose = false;
};

std::vector<std::string> parse_symbols(const std::string& csv) {
    std::vector<std::string> symbols;
    std::size_t start = 0;
    while (start < csv.size()) {
        const auto end = csv.find(',', start);
        const auto length = (end == std::string::npos ? csv.size() : end) - start;
        const auto symbol = csv.substr(start, length);
        if (symbol.empty()) {
            throw std::invalid_argument("symbol list contains an empty symbol");
        }
        if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) {
            symbols.push_back(symbol);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (symbols.empty() || symbols.size() > kMaximumConfiguredSymbols) {
        throw std::invalid_argument("configure between 1 and 65535 symbols");
    }
    return symbols;
}

std::uint16_t parse_fee(const std::string& text, const char* option) {
    const auto fee = std::stoul(text);
    if (fee > kFeeScale) {
        throw std::invalid_argument(std::string(option) +
                                    " must be in the range 0-10000 bps");
    }
    return static_cast<std::uint16_t>(fee);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") +
                                            name);
            }
            return argv[++i];
        };
        if (arg == "--source") {
            options.source = value("--source");
        } else if (arg == "--socket") {
            options.socket = value("--socket");
        } else if (arg == "--symbols") {
            options.symbols = parse_symbols(value("--symbols"));
        } else if (arg == "--bybit-fee-bps") {
            options.bybit_fee_bps = parse_fee(value("--bybit-fee-bps"),
                                               "--bybit-fee-bps");
        } else if (arg == "--binance-fee-bps") {
            options.binance_fee_bps = parse_fee(value("--binance-fee-bps"),
                                                 "--binance-fee-bps");
        } else if (arg == "--min-profit") {
            const auto profit = std::stod(value("--min-profit"));
            const long double scaled =
                static_cast<long double>(profit) * kPriceScale * kFeeScale;
            if (!std::isfinite(profit) || profit < 0.0 ||
                scaled > ((std::uint64_t{1} << 49) - 1)) {
                throw std::invalid_argument(
                    "--min-profit must be nonnegative and fit RTL width");
            }
            options.min_profit = static_cast<std::uint64_t>(scaled + 0.5L);
        } else if (arg == "--max-quote-age-ms") {
            const auto age = std::stoul(value("--max-quote-age-ms"));
            if (age == 0 || age > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument(
                    "--max-quote-age-ms must be between 1 and 4294967295");
            }
            options.max_quote_age_ms = static_cast<std::uint32_t>(age);
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: market_data_engine [--source abides|websocket] "
                   "[--symbols BTCUSDT,ETHUSDT] [--socket PATH] "
                   "[--bybit-fee-bps N] [--binance-fee-bps N] "
                   "[--min-profit PRICE] [--max-quote-age-ms N] [--verbose]\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }
    if (options.source != "abides" && options.source != "websocket") {
        throw std::invalid_argument("--source must be abides or websocket");
    }
    return options;
}

class RollingWindow {
public:
    explicit RollingWindow(std::size_t capacity) : capacity_(capacity) {}

    double zscore(double current) const {
        if (values_.size() < 2) {
            return 0.0;
        }
        double sum = 0.0;
        for (double value : values_) {
            sum += value;
        }
        const double mean = sum / static_cast<double>(values_.size());
        double squared_sum = 0.0;
        for (double value : values_) {
            const double difference = value - mean;
            squared_sum += difference * difference;
        }
        const double deviation =
            std::sqrt(squared_sum / static_cast<double>(values_.size() - 1));
        return deviation == 0.0 ? 0.0 : (current - mean) / deviation;
    }

    void push(double value) {
        values_.push_back(value);
        if (values_.size() > capacity_) {
            values_.pop_front();
        }
    }

private:
    std::size_t capacity_;
    std::deque<double> values_;
};

struct Decision {
    std::string symbol;
    std::uint64_t cycle;
    bool candidate;
    std::uint8_t direction;
    std::int64_t net_profit;
    std::uint32_t bybit_bid;
    std::uint32_t bybit_ask;
    std::uint32_t binance_bid;
    std::uint32_t binance_ask;
    std::uint16_t bybit_fee_bps;
    std::uint16_t binance_fee_bps;
    std::chrono::steady_clock::time_point enqueued_at{};
};

struct PaperPosition {
    std::uint8_t direction;
    std::uint32_t bybit_ask;
    std::uint32_t bybit_bid;
    std::uint32_t binance_bid;
    std::uint32_t binance_ask;
    std::uint32_t mark_bybit_bid;
    std::uint32_t mark_bybit_ask;
    std::uint32_t mark_binance_bid;
    std::uint32_t mark_binance_ask;
    std::uint16_t bybit_fee_bps;
    std::uint16_t binance_fee_bps;
};

class DecisionWorker {
public:
    DecisionWorker(const std::vector<std::string>& symbols, bool verbose,
                   std::mutex& output_mutex, RuntimeMetrics& metrics)
        : symbols_(symbols), verbose_(verbose), output_mutex_(output_mutex),
          metrics_(metrics) {
        windows_.reserve(symbols.size());
        positions_.resize(symbols.size());
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            windows_.emplace_back(50);
        }
        thread_ = std::thread(&DecisionWorker::run, this);
    }

    ~DecisionWorker() {
        stop();
    }

    void submit(Decision decision) {
        decision.enqueued_at = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() == kDecisionQueueCapacity) {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(std::move(decision));
        available_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        available_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        std::lock_guard<std::mutex> output_lock(output_mutex_);
        for (std::size_t i = 0; i < positions_.size(); ++i) {
            if (!positions_[i]) {
                continue;
            }
            const auto& position = *positions_[i];
            std::cout << "[TRADE] stopped out paper pair symbol="
                      << symbols_[i] << " direction="
                      << (position.direction == 1
                              ? "BUY_BYBIT_SELL_BINANCE"
                              : "BUY_BINANCE_SELL_BYBIT")
                      << " quantity=" << Config{}.position_size
                      << " realized_pnl=" << std::fixed
                      << std::setprecision(4)
                      << realized_pnl(position, position.mark_bybit_bid,
                                      position.mark_bybit_ask,
                                      position.mark_binance_bid,
                                      position.mark_binance_ask)
                      << " reason=application_shutdown\n";
            positions_[i].reset();
        }
    }

    std::size_t dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    static double realized_pnl(const PaperPosition& position,
                               std::uint32_t bybit_bid,
                               std::uint32_t bybit_ask,
                               std::uint32_t binance_bid,
                               std::uint32_t binance_ask) {
        const auto bybit_fee =
            static_cast<double>(position.bybit_fee_bps) / kFeeScale;
        const auto binance_fee =
            static_cast<double>(position.binance_fee_bps) / kFeeScale;
        double entry_spread;
        double exit_spread;
        if (position.direction == 1) {
            entry_spread =
                position.binance_bid * (1.0 - binance_fee) -
                position.bybit_ask * (1.0 + bybit_fee);
            exit_spread = bybit_bid * (1.0 - bybit_fee) -
                          binance_ask * (1.0 + binance_fee);
        } else {
            entry_spread =
                position.bybit_bid * (1.0 - bybit_fee) -
                position.binance_ask * (1.0 + binance_fee);
            exit_spread = binance_bid * (1.0 - binance_fee) -
                          bybit_ask * (1.0 + bybit_fee);
        }
        return (entry_spread + exit_spread) /
               static_cast<double>(kPriceScale);
    }

    void run() {
        while (true) {
            Decision decision;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                available_.wait(lock, [this] {
                    return stopping_ || !queue_.empty();
                });
                if (queue_.empty()) {
                    return;
                }
                decision = std::move(queue_.front());
                queue_.pop_front();
            }
            metrics_.strategy_queue_wait.add(
                std::chrono::steady_clock::now() - decision.enqueued_at);

            const auto symbol = symbol_ids_.find(decision.symbol);
            if (symbol == symbol_ids_.end()) {
                continue;
            }
            const double profit =
                static_cast<double>(decision.net_profit) / 100'000'000.0;
            auto& window = windows_[symbol->second];
            const double zscore = window.zscore(profit);
            window.push(profit);

            // Verilator's positive after-fee result is the strategy signal.
            // Keep z-score as diagnostic context, but do not gate this signal.
            const bool accepted = decision.candidate;
            const bool show_decision =
                verbose_ || (!decision.candidate && printed_rejects_ < 10);
            if (!decision.candidate) {
                ++printed_rejects_;
            }

            std::lock_guard<std::mutex> output_lock(output_mutex_);
            const char* direction =
                decision.direction == 1 ? "BUY_BYBIT_SELL_BINANCE"
                : decision.direction == 2 ? "BUY_BINANCE_SELL_BYBIT"
                                           : "NONE";
            if (show_decision) {
                std::cout << "[DECISION] symbol=" << decision.symbol
                          << " result=" << (accepted ? "ACCEPT" : "REJECT")
                          << " reason="
                          << (accepted ? "VERILATOR_PROFITABLE_SPREAD"
                                       : "RTL_NO_CANDIDATE")
                          << " zscore=" << zscore;
                if (decision.candidate) {
                    std::cout << " direction=" << direction
                              << " spread="
                              << static_cast<double>(decision.net_profit) /
                                     100'000'000.0;
                }
                if (accepted) {
                    std::cout << " quantity=" << Config{}.position_size;
                }
                std::cout << '\n';
            }

            auto& position = positions_[symbol->second];
            if (position &&
                (!decision.candidate ||
                 position->direction != decision.direction)) {
                std::cout << "[TRADE] stopped out paper pair symbol="
                          << decision.symbol
                          << " direction="
                          << (position->direction == 1
                                  ? "BUY_BYBIT_SELL_BINANCE"
                                  : "BUY_BINANCE_SELL_BYBIT")
                          << " quantity=" << Config{}.position_size
                          << " realized_pnl=" << std::fixed
                          << std::setprecision(4)
                          << realized_pnl(*position, decision.bybit_bid,
                                          decision.bybit_ask,
                                          decision.binance_bid,
                                          decision.binance_ask)
                          << " reason="
                          << (decision.candidate ? "opportunity_reversed"
                                                : "RTL_candidate_cleared")
                          << '\n';
                position.reset();
            }

            if (decision.candidate && !position) {
                position = PaperPosition{decision.direction,
                                         decision.bybit_ask,
                                         decision.bybit_bid,
                                         decision.binance_bid,
                                         decision.binance_ask,
                                         decision.bybit_bid,
                                         decision.bybit_ask,
                                         decision.binance_bid,
                                         decision.binance_ask,
                                         decision.bybit_fee_bps,
                                         decision.binance_fee_bps};
                std::cout << "[TRADE] entered paper pair symbol="
                          << decision.symbol << " direction=" << direction
                          << " quantity=" << Config{}.position_size
                          << " buy_price="
                          << std::fixed << std::setprecision(4)
                          << (decision.direction == 1 ? decision.bybit_ask
                                                      : decision.binance_ask) /
                                 static_cast<double>(kPriceScale)
                          << " sell_price="
                          << (decision.direction == 1 ? decision.binance_bid
                                                      : decision.bybit_bid) /
                                 static_cast<double>(kPriceScale)
                          << " estimated_net_profit="
                          << static_cast<double>(decision.net_profit) /
                                 (kPriceScale * kFeeScale)
                          << '\n';
            }
            if (decision.candidate && position) {
                position->mark_bybit_bid = decision.bybit_bid;
                position->mark_bybit_ask = decision.bybit_ask;
                position->mark_binance_bid = decision.binance_bid;
                position->mark_binance_ask = decision.binance_ask;
            }
        }
    }

public:
    void set_symbol_ids(const std::unordered_map<std::string, std::size_t>& ids) {
        symbol_ids_ = ids;
    }

private:
    std::vector<RollingWindow> windows_;
    std::vector<std::optional<PaperPosition>> positions_;
    std::vector<std::string> symbols_;
    bool verbose_;
    std::mutex& output_mutex_;
    RuntimeMetrics& metrics_;
    std::unordered_map<std::string, std::size_t> symbol_ids_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Decision> queue_;
    bool stopping_ = false;
    std::atomic<std::size_t> dropped_{0};
    std::size_t printed_rejects_ = 0;
    std::thread thread_;
};

struct AbidesQuote {
    std::string symbol;
    std::uint32_t bid;
    std::uint32_t ask;
};

std::uint32_t read_u32(const char* data) {
    std::uint32_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::int64_t read_i64(const char* data) {
    std::int64_t value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::vector<AbidesQuote> parse_abides_packet(const std::string& payload) {
    std::vector<AbidesQuote> quotes;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        constexpr std::size_t header_size = 44;
        if (payload.size() - offset < header_size) {
            throw std::runtime_error("truncated ABIDES symbol record");
        }
        char name[17]{};
        std::memcpy(name, payload.data() + offset, 16);
        offset += 16;
        const auto level_count = read_u32(payload.data() + offset);
        offset += 4;
        const auto bid = read_i64(payload.data() + offset);
        offset += 8;
        const auto ask = read_i64(payload.data() + offset);
        offset += 8;
        (void)read_i64(payload.data() + offset);
        offset += 8;
        if (level_count > (payload.size() - offset) / 16) {
            throw std::runtime_error("invalid ABIDES order-book depth");
        }
        offset += static_cast<std::size_t>(level_count) * 16;
        if (bid <= 0 || ask <= 0) {
            continue;
        }
        constexpr std::int64_t abides_scale_per_rtl_unit = 10'000;
        const auto bid_rtl =
            (bid + abides_scale_per_rtl_unit / 2) / abides_scale_per_rtl_unit;
        const auto ask_rtl =
            (ask + abides_scale_per_rtl_unit / 2) / abides_scale_per_rtl_unit;
        if (bid_rtl > UINT32_MAX || ask_rtl > UINT32_MAX) {
            continue;
        }
        quotes.push_back({name, static_cast<std::uint32_t>(bid_rtl),
                          static_cast<std::uint32_t>(ask_rtl)});
    }
    return quotes;
}

std::string direction_name(unsigned direction) {
    if (direction == 1) {
        return "BUY_BYBIT_SELL_BINANCE";
    }
    if (direction == 2) {
        return "BUY_BINANCE_SELL_BYBIT";
    }
    return "NONE";
}

std::int64_t signed_net_profit(const Vspread_detector& rtl) {
    constexpr std::uint64_t mask = (std::uint64_t{1} << 49) - 1;
    const std::uint64_t raw =
        static_cast<std::uint64_t>(rtl.net_profit_o) & mask;
    return static_cast<std::int64_t>(
        (raw ^ (std::uint64_t{1} << 48)) - (std::uint64_t{1} << 48));
}

} // namespace

int main(int argc, char** argv) {
    std::ofstream log_file("market_data_engine.txt",
                           std::ios::out | std::ios::trunc);
    if (!log_file) {
        std::cerr << "[FATAL] cannot create market_data_engine.txt\n";
        return EXIT_FAILURE;
    }
    TeeStreamBuffer output_buffer(std::cout.rdbuf(), log_file.rdbuf());
    std::cout.rdbuf(&output_buffer);

    try {
        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;
        const Options options = parse_options(argc, argv);
        std::mutex output_mutex;
        std::unordered_map<std::string, std::size_t> symbol_ids;
        for (std::size_t i = 0; i < options.symbols.size(); ++i) {
            symbol_ids.emplace(options.symbols[i], i);
        }
        std::cout << "[SOURCE] mode=" << options.source
                  << " symbols=" << options.symbols.size() << '\n';
        std::cout << "[RTL] prices=uint32_fixed_1e4 fees=bps min_profit="
                  << options.min_profit << " registered_latency=1_cycle\n";
        std::cout << "[STRATEGY] mode=paper quantity="
                  << Config{}.position_size
                  << " bybit_fee_bps=" << options.bybit_fee_bps
                  << " binance_fee_bps=" << options.binance_fee_bps << '\n';

        std::signal(SIGINT, signal_handler);
        auto rtl = std::make_unique<Vspread_detector>();
        rtl->clk = 0;
        rtl->rst_n = 0;
        rtl->input_valid = 0;
        rtl->quotes_valid = 0;
        rtl->bybit_bid_i = 0;
        rtl->bybit_ask_i = 0;
        rtl->binance_bid_i = 0;
        rtl->binance_ask_i = 0;
        rtl->bybit_fee_bps_i = 0;
        rtl->binance_fee_bps_i = 0;
        rtl->min_profit_i = options.min_profit;
        rtl->symbol_id_i = 0;
        rtl->transaction_id_i = 0;
        rtl->eval();
        rtl->clk = 1;
        rtl->eval();
        rtl->clk = 0;
        rtl->rst_n = 1;
        rtl->eval();

        RuntimeMetrics runtime_metrics;
        DecisionWorker decision_worker(options.symbols, options.verbose,
                                       output_mutex, runtime_metrics);
        decision_worker.set_symbol_ids(symbol_ids);

        std::unique_ptr<UdsSocketReader> abides;
        std::unique_ptr<WebSocketFeed> websocket;
        if (options.source == "abides") {
            abides = std::make_unique<UdsSocketReader>(options.socket);
            if (!abides->connected()) {
                throw std::runtime_error(
                    "cannot connect to ABIDES socket " + options.socket +
                    "; start exchange/abides/exchange_simulator.py first");
            }
            std::cout << "[SOURCE] connected to ABIDES socket="
                      << options.socket << '\n';
        } else {
            websocket = std::make_unique<WebSocketFeed>(options.symbols);
            websocket->start();
            std::cout << "[SOURCE] connecting to public Bybit and Binance "
                         "WebSockets\n";
        }

        struct VenueQuote {
            std::uint32_t bid = 0;
            std::uint32_t ask = 0;
            std::int64_t exchange_time_ms = 0;
            std::chrono::steady_clock::time_point received_at{};
        };
        struct PairState {
            VenueQuote bybit;
            VenueQuote binance;
        };
        std::vector<PairState> pairs(options.symbols.size());
        std::vector<std::uint8_t> alerted_directions(options.symbols.size(), 0);
        std::uint64_t cycle = 0;
        std::uint32_t transaction_id = 0;
        std::uint64_t updates = 0;
        std::uint64_t candidates = 0;
        std::uint64_t stale_quotes = 0;
        std::uint64_t malformed_abides = 0;
        BenchmarkReport benchmark;
        const auto wall_now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
        const auto feed_rtl = [&](std::size_t symbol_index,
                                  const VenueQuote& bybit_quote,
                                  const VenueQuote& binance_quote,
                                  bool quotes_valid,
                                  std::chrono::steady_clock::time_point
                                      message_received_at) {
            const auto symbol_id =
                static_cast<std::uint16_t>(symbol_index + 1);
            ++transaction_id;
            const auto rtl_started_at = std::chrono::steady_clock::now();
            rtl->clk = 0;
            rtl->input_valid = 1;
            rtl->quotes_valid = quotes_valid;
            rtl->bybit_bid_i = bybit_quote.bid;
            rtl->bybit_ask_i = bybit_quote.ask;
            rtl->binance_bid_i = binance_quote.bid;
            rtl->binance_ask_i = binance_quote.ask;
            rtl->bybit_fee_bps_i = options.bybit_fee_bps;
            rtl->binance_fee_bps_i = options.binance_fee_bps;
            rtl->min_profit_i = options.min_profit;
            rtl->symbol_id_i = symbol_id;
            rtl->transaction_id_i = transaction_id;
            rtl->eval();
            rtl->clk = 1;
            rtl->eval();
            const auto rtl_completed_at = std::chrono::steady_clock::now();
            runtime_metrics.rtl_eval_wall_time.add(rtl_completed_at -
                                                   rtl_started_at);
            runtime_metrics.websocket_callback_to_rtl_output.add(
                rtl_completed_at - message_received_at);
            ++cycle;
            ++updates;

            const bool candidate = rtl->candidate_valid_o != 0;
            const auto direction =
                static_cast<std::uint8_t>(rtl->direction_o);
            const bool new_candidate =
                candidate && alerted_directions[symbol_index] != direction;
            if (!candidate) {
                alerted_directions[symbol_index] = 0;
            } else if (new_candidate) {
                alerted_directions[symbol_index] = direction;
            }
            const auto profit = signed_net_profit(*rtl);
            if (candidate) {
                ++candidates;
            }
            if (options.verbose || updates <= 10 || new_candidate) {
                std::lock_guard<std::mutex> output_lock(output_mutex);
                std::cout << "[RTL] cycle=" << cycle
                          << " symbol=" << options.symbols[symbol_index]
                          << " symbol_id=" << symbol_id
                          << " transaction_id=" << transaction_id
                          << " input_valid=1 quotes_valid="
                          << static_cast<unsigned>(rtl->quotes_valid)
                          << " bybit_bid=" << std::fixed
                          << std::setprecision(4)
                          << static_cast<double>(rtl->bybit_bid_i) /
                                 kPriceScale
                          << " bybit_ask="
                          << static_cast<double>(rtl->bybit_ask_i) /
                                 kPriceScale
                          << " binance_bid="
                          << static_cast<double>(rtl->binance_bid_i) /
                                 kPriceScale
                          << " binance_ask="
                          << static_cast<double>(rtl->binance_ask_i) /
                                 kPriceScale
                          << " candidate_valid="
                          << static_cast<unsigned>(rtl->candidate_valid_o)
                          << " direction="
                          << direction_name(rtl->direction_o)
                          << " net_profit_raw=" << profit
                          << " net_profit_usd=" << std::fixed
                          << std::setprecision(4)
                          << static_cast<double>(profit) /
                                 (kPriceScale * kFeeScale)
                          << '\n';
            }
            if (new_candidate) {
                std::lock_guard<std::mutex> output_lock(output_mutex);
                std::cout << "[ALERT] Verilator signaled to C++ about "
                             "arbitrageable spread"
                          << " symbol=" << options.symbols[symbol_index]
                          << " direction=" << direction_name(rtl->direction_o)
                          << " cycle=" << cycle
                          << " transaction_id=" << transaction_id
                          << " net_profit="
                          << static_cast<double>(profit) /
                                 (kPriceScale * kFeeScale)
                          << '\n';
            }
            decision_worker.submit(
                {options.symbols[symbol_index], cycle, candidate,
                 static_cast<std::uint8_t>(rtl->direction_o), profit,
                 bybit_quote.bid, bybit_quote.ask, binance_quote.bid,
                 binance_quote.ask, options.bybit_fee_bps,
                 options.binance_fee_bps});
            runtime_metrics.websocket_callback_to_strategy_enqueue.add(
                std::chrono::steady_clock::now() - message_received_at);
            rtl->clk = 0;
            rtl->eval();
        };
        const auto accept_market_update = [&](const QuoteUpdate& update) {
            const auto symbol = symbol_ids.find(update.symbol);
            if (symbol == symbol_ids.end()) {
                return;
            }
            auto& pair = pairs[symbol->second];
            auto& quote =
                update.venue == Venue::Bybit ? pair.bybit : pair.binance;
            quote = {update.bid, update.ask, update.exchange_time_ms,
                     update.received_at};
            if (pair.bybit.bid == 0 || pair.binance.bid == 0) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto bybit_age = now - pair.bybit.received_at;
            const auto binance_age = now - pair.binance.received_at;
            const auto max_age =
                std::chrono::milliseconds(options.max_quote_age_ms);
            // Exchange event clocks can differ by seconds; receipt age uses
            // this process's monotonic clock and is the reliable liveness test.
            const bool fresh = bybit_age <= max_age && binance_age <= max_age;
            if (!fresh) {
                ++stale_quotes;
                return;
            }
            feed_rtl(symbol->second, pair.bybit, pair.binance, true,
                     update.received_at);
        };

        std::cout << "[SOURCE] mode=" << options.source << " ready\n";
        const auto engine_started_at = std::chrono::steady_clock::now();
        auto last_status = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_relaxed)) {
            if (options.source == "abides") {
                std::string payload;
                std::error_code error;
                if (abides->read_packet(payload, error) < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                try {
                    for (const auto& quote : parse_abides_packet(payload)) {
                        const auto symbol = symbol_ids.find(quote.symbol);
                        if (symbol == symbol_ids.end()) {
                            continue;
                        }
                        const VenueQuote simulated{
                            quote.bid, quote.ask, wall_now_ms(),
                            std::chrono::steady_clock::now()};
                        feed_rtl(symbol->second, simulated, simulated,
                                 quote.bid < quote.ask,
                                 simulated.received_at);
                    }
                } catch (const std::exception& error) {
                    ++malformed_abides;
                    if (options.verbose || malformed_abides <= 10) {
                        std::cerr << "[SOURCE] malformed ABIDES packet: "
                                  << error.what() << '\n';
                    }
                }
            } else {
                QuoteUpdate update{};
                if (websocket->wait_for_update(update,
                                               std::chrono::milliseconds(100))) {
                    accept_market_update(update);
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_status >= std::chrono::seconds(5)) {
                std::lock_guard<std::mutex> output_lock(output_mutex);
                std::cout << "[STATUS] updates=" << updates
                          << " candidates=" << candidates
                          << " stale_quotes=" << stale_quotes
                          << " decision_queue_dropped="
                          << decision_worker.dropped();
benchmark.total_updates = updates;
                benchmark.total_candidates = candidates;
                benchmark.stale_quotes = stale_quotes;
                benchmark.malformed_packets = malformed_abides;
                benchmark.decision_queue_dropped = decision_worker.dropped();
                if (websocket) {
                    benchmark.websocket_dropped = websocket->dropped_updates();
                    benchmark.websocket_malformed = websocket->malformed_messages();
                }
                std::cout << " " << benchmark.format();
                if (websocket) {
                    std::cout << " websocket_queue_dropped="
                              << websocket->dropped_updates()
                              << " malformed_messages="
                              << websocket->malformed_messages();
                }
                std::cout << '\n';
                last_status = now;
            }
        }
        const auto engine_finished_at = std::chrono::steady_clock::now();

        if (websocket) {
            std::cout << "[SOURCE] stopping WebSocket readers\n";
            websocket->stop();
            std::cout << "[SOURCE] WebSocket readers stopped\n";
        }
        std::cout << "[SOURCE] draining decision worker\n";
        decision_worker.stop();
        std::cout << "[SOURCE] decision worker stopped\n";
        if (abides) {
            abides->close();
        }
        const double runtime_seconds = std::chrono::duration<double>(
                                           engine_finished_at -
                                           engine_started_at)
                                           .count();
        std::ofstream benchmark_file("market_data_engine_benchmark.txt",
                                     std::ios::out | std::ios::trunc);
        if (benchmark_file) {
            benchmark_file << "Market Data Engine Runtime Benchmark\n"
                           << "=====================================\n"
                           << "Source: " << options.source << '\n'
                           << "Symbols: " << options.symbols.size() << '\n'
                           << "Runtime seconds: " << std::fixed
                           << std::setprecision(3) << runtime_seconds << '\n'
                           << "Accepted RTL updates: " << updates << '\n'
                           << "RTL candidates: " << candidates << '\n'
                           << "Throughput updates_per_second: "
                           << (runtime_seconds > 0.0
                                   ? updates / runtime_seconds
                                   : 0.0)
                           << "\nStale quote rejections: " << stale_quotes
                           << "\nWebSocket dropped updates: "
                           << (websocket ? websocket->dropped_updates() : 0)
                           << "\nWebSocket malformed messages: "
                           << (websocket ? websocket->malformed_messages() : 0)
                           << "\nDecision queue drops: "
                           << decision_worker.dropped()
                           << "\n\nMeasured software latency (steady_clock, ns):\n"
                           << "WebSocket callback to Verilator output: "
                           << runtime_metrics.websocket_callback_to_rtl_output
                                  .summary()
                           << "\nVerilator eval wall time: "
                           << runtime_metrics.rtl_eval_wall_time.summary()
                           << "\nWebSocket callback to strategy enqueue: "
                           << runtime_metrics
                                  .websocket_callback_to_strategy_enqueue
                                  .summary()
                           << "\nStrategy queue wait: "
                           << runtime_metrics.strategy_queue_wait.summary()
                           << "\n\nHardware and network evidence:\n"
                           << "RTL registered latency: 1 rising-edge cycle in the Verilated model; no physical clock period measured\n"
                           << "FPGA target / bitstream: not configured\n"
                           << "Post-place-and-route Fmax: not measured\n"
                           << "WNS / WHS timing closure: not measured\n"
                           << "FPGA LUT / FF / carry / BRAM / URAM / DSP usage: not measured for a target device\n"
                           << "Fiber receive / transmit latency: not measured; callback timing starts after the WebSocket read completes\n"
                           << "Order dispatch latency: not measured; this executable emits paper decisions and sends no exchange orders\n"
                           << "mmap / FPGA DMA: not present; Verilator is linked into this process\n";
            std::cout << "[BENCHMARK] report=market_data_engine_benchmark.txt\n";
        } else {
            std::cerr << "[WARN] cannot create market_data_engine_benchmark.txt\n";
        }
        std::cout << "[STOP] updates=" << updates
                  << " candidates=" << candidates
                  << " stale_quotes=" << stale_quotes
                  << " decision_queue_dropped=" << decision_worker.dropped();
benchmark.total_updates = updates;
                benchmark.total_candidates = candidates;
                benchmark.stale_quotes = stale_quotes;
                benchmark.malformed_packets = malformed_abides;
                benchmark.decision_queue_dropped = decision_worker.dropped();
                if (websocket) {
                    benchmark.websocket_dropped = websocket->dropped_updates();
                    benchmark.websocket_malformed = websocket->malformed_messages();
                }
                std::cout << " " << benchmark.format();
        if (websocket) {
            std::cout << " websocket_queue_dropped="
                      << websocket->dropped_updates()
                      << " malformed_messages="
                      << websocket->malformed_messages();
        }
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        std::cout.rdbuf(output_buffer.console_buffer());
        return EXIT_FAILURE;
    }
    std::cout.rdbuf(output_buffer.console_buffer());
    return EXIT_SUCCESS;
}
