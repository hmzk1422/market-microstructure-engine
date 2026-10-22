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
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

std::atomic<bool> running{true};

void signal_handler(int) {
    running.store(false, std::memory_order_relaxed);
}

struct Options {
    std::string source = "abides";
    std::string socket = Config{}.socket_path;
    std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT"};
    std::uint16_t bybit_fee_bps = 10;
    std::uint16_t binance_fee_bps = 10;
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
};

class DecisionWorker {
public:
    DecisionWorker(const std::vector<std::string>& symbols, bool verbose,
                   std::mutex& output_mutex)
        : verbose_(verbose), output_mutex_(output_mutex) {
        windows_.reserve(symbols.size());
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            windows_.emplace_back(50);
        }
        thread_ = std::thread(&DecisionWorker::run, this);
    }

    ~DecisionWorker() {
        stop();
    }

    void submit(Decision decision) {
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
    }

    std::size_t dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
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

            const auto symbol = symbol_ids_.find(decision.symbol);
            if (symbol == symbol_ids_.end()) {
                continue;
            }
            const double profit =
                static_cast<double>(decision.net_profit) / 100'000'000.0;
            auto& window = windows_[symbol->second];
            const double zscore = window.zscore(profit);
            window.push(profit);

            const bool accepted =
                decision.candidate && std::abs(zscore) >= Config{}.zscore_threshold;
            if (!verbose_ && !decision.candidate && printed_rejects_ >= 10) {
                continue;
            }
            if (!decision.candidate) {
                ++printed_rejects_;
            }

            std::lock_guard<std::mutex> output_lock(output_mutex_);
            const char* direction =
                decision.direction == 1 ? "BUY_BYBIT_SELL_BINANCE"
                : decision.direction == 2 ? "BUY_BINANCE_SELL_BYBIT"
                                           : "NONE";
            std::cout << "[DECISION] symbol=" << decision.symbol
                      << " result=" << (accepted ? "ACCEPT" : "REJECT")
                      << " reason="
                      << (accepted ? "Z_SCORE_THRESHOLD_MET"
                          : decision.candidate ? "Z_SCORE_BELOW_THRESHOLD"
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
    }

public:
    void set_symbol_ids(const std::unordered_map<std::string, std::size_t>& ids) {
        symbol_ids_ = ids;
    }

private:
    std::vector<RollingWindow> windows_;
    bool verbose_;
    std::mutex& output_mutex_;
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

        DecisionWorker decision_worker(options.symbols, options.verbose,
                                       output_mutex);
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
        std::uint64_t cycle = 0;
        std::uint32_t transaction_id = 0;
        std::uint64_t updates = 0;
        std::uint64_t candidates = 0;
        std::uint64_t stale_quotes = 0;
        std::uint64_t malformed_abides = 0;
        const auto wall_now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
        const auto feed_rtl = [&](std::size_t symbol_index,
                                  const VenueQuote& bybit_quote,
                                  const VenueQuote& binance_quote,
                                  bool quotes_valid) {
            const auto symbol_id =
                static_cast<std::uint16_t>(symbol_index + 1);
            ++transaction_id;
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
            ++cycle;
            ++updates;

            const bool candidate = rtl->candidate_valid_o != 0;
            const auto profit = signed_net_profit(*rtl);
            if (candidate) {
                ++candidates;
            }
            if (options.verbose || updates <= 10 || candidate) {
                std::lock_guard<std::mutex> output_lock(output_mutex);
                std::cout << "[RTL] cycle=" << cycle
                          << " symbol=" << options.symbols[symbol_index]
                          << " symbol_id=" << symbol_id
                          << " transaction_id=" << transaction_id
                          << " input_valid=1 quotes_valid="
                          << static_cast<unsigned>(rtl->quotes_valid)
                          << " bybit_bid=" << rtl->bybit_bid_i
                          << " bybit_ask=" << rtl->bybit_ask_i
                          << " binance_bid=" << rtl->binance_bid_i
                          << " binance_ask=" << rtl->binance_ask_i
                          << " candidate_valid="
                          << static_cast<unsigned>(rtl->candidate_valid_o)
                          << " direction="
                          << direction_name(rtl->direction_o)
                          << " net_profit=" << profit << '\n';
            }
            decision_worker.submit(
                {options.symbols[symbol_index], cycle, candidate,
                 static_cast<std::uint8_t>(rtl->direction_o), profit});
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
            const bool fresh = bybit_age <= max_age && binance_age <= max_age &&
                               std::llabs(wall_now_ms() -
                                          pair.bybit.exchange_time_ms) <=
                                   options.max_quote_age_ms &&
                               std::llabs(wall_now_ms() -
                                          pair.binance.exchange_time_ms) <=
                                   options.max_quote_age_ms;
            if (!fresh) {
                ++stale_quotes;
                return;
            }
            feed_rtl(symbol->second, pair.bybit, pair.binance, true);
        };

        std::cout << "[SOURCE] mode=" << options.source << " ready\n";
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
                                 quote.bid < quote.ask);
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

        if (websocket) {
            websocket->stop();
        }
        decision_worker.stop();
        if (abides) {
            abides->close();
        }
        std::cout << "[STOP] updates=" << updates
                  << " candidates=" << candidates
                  << " stale_quotes=" << stale_quotes
                  << " decision_queue_dropped=" << decision_worker.dropped();
        if (websocket) {
            std::cout << " websocket_queue_dropped="
                      << websocket->dropped_updates()
                      << " malformed_messages="
                      << websocket->malformed_messages();
        }
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << "[FATAL] " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
