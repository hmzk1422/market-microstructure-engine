#include "Vspread_detector.h"
#include "verilated.h"

#include "../../exchange/config.h"
#include "../../exchange/market_data.h"
#include "../../exchange/socket_reader.h"
#include "../../exchange/websocket_feed.h"
#include "../../exchange/spsc_ring.h"
#include "../../exchange/mmap_spsc_ring.h"
#include "../../exchange/thread_affinity.h"
#include "../metrics/timescale_telemetry.h"
#include "strategy_core.h"
#include "network_latency_model.h"
#include "spread_math.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <ios>
#include <streambuf>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sstream>
#include <iostream>
#include <map>
#include <array>

#ifdef _WIN32
#include <csignal>
#include <windows.h>
#include <psapi.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

constexpr std::uint64_t kPriceScale = 10'000;
constexpr std::uint64_t kFeeScale = 10'000;
constexpr std::size_t kDecisionQueueCapacity = 4096;
constexpr std::size_t kMaximumConfiguredSymbols = 65'534;

struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

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

    std::streamsize xsputn(const char* data, std::streamsize size) override {
        const auto console_written = console_->sputn(data, size);
        const auto file_written = file_->sputn(data, size);
        return std::min(console_written, file_written);
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
        const auto ns = static_cast<std::uint64_t>(std::max<std::int64_t>(0, nanoseconds));
        total_ns_.fetch_add(ns, std::memory_order_relaxed);
        auto previous_max = max_ns_.load(std::memory_order_relaxed);
        while (previous_max < ns && !max_ns_.compare_exchange_weak(
                   previous_max, ns, std::memory_order_relaxed)) {}
        histogram_[bucket_for(ns)].fetch_add(1, std::memory_order_relaxed);
        observed_.fetch_add(1, std::memory_order_release);
    }

    std::string summary() const {
        const auto observed = observed_.load(std::memory_order_acquire);
        if (observed == 0) {
            return "no samples";
        }
        const double mean = static_cast<double>(
            total_ns_.load(std::memory_order_relaxed)) / observed;
        std::ostringstream output;
        output << "observed=" << observed_
               << std::fixed << std::setprecision(0) << " mean_ns=" << mean
               << " p05_ns=" << percentile_unlocked(0.05)
               << " p50_ns=" << percentile_unlocked(0.50)
               << " p95_ns=" << percentile_unlocked(0.95)
               << " p99_ns=" << percentile_unlocked(0.99)
               << " max_ns=" << max_ns_.load(std::memory_order_relaxed);
        return output.str();
    }

    double percentile(double fraction) const {
        if (observed_.load(std::memory_order_acquire) == 0)
            return std::numeric_limits<double>::quiet_NaN();
        return percentile_unlocked(fraction);
    }

    double maximum() const noexcept {
        if (observed_.load(std::memory_order_acquire) == 0)
            return std::numeric_limits<double>::quiet_NaN();
        return static_cast<double>(max_ns_.load(std::memory_order_relaxed));
    }

    std::uint64_t count() const noexcept {
        return observed_.load(std::memory_order_acquire);
    }

private:
    static std::size_t bucket_for(std::uint64_t ns) {
        if (ns < 1'000'000) return static_cast<std::size_t>(ns / 1'000);
        if (ns < 100'000'000) {
            return 1'000 + static_cast<std::size_t>(
                                (ns - 1'000'000) / 10'000);
        }
        if (ns < 1'000'000'000) {
            return 10'900 + static_cast<std::size_t>(
                                 (ns - 100'000'000) / 100'000);
        }
        return 19'900;
    }

    double bucket_value(std::size_t bucket) const {
        if (bucket < 1'000) return static_cast<double>(bucket) * 1'000.0 + 500.0;
        if (bucket < 10'900) return 1'000'000.0 +
            static_cast<double>(bucket - 1'000) * 10'000.0 + 5'000.0;
        if (bucket < 19'900) return 100'000'000.0 +
            static_cast<double>(bucket - 10'900) * 100'000.0 + 50'000.0;
        return static_cast<double>(max_ns_.load(std::memory_order_relaxed));
    }

    double percentile_unlocked(double fraction) const {
        const auto observed = observed_.load(std::memory_order_acquire);
        const auto target = static_cast<std::uint64_t>(
            std::clamp(fraction, 0.0, 1.0) * (observed - 1)) + 1;
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < histogram_.size(); ++i) {
            cumulative += histogram_[i].load(std::memory_order_relaxed);
            if (cumulative >= target) return bucket_value(i);
        }
        return static_cast<double>(max_ns_);
    }

    // 1 us resolution below 1 ms, 10 us up to 100 ms, then 100 us up to 1 s.
    // This resolves the historical 62 us tail while keeping fixed memory use.
    static constexpr std::size_t kHistogramBuckets = 19'901;
    std::array<std::atomic<std::uint64_t>, kHistogramBuckets> histogram_{};
    std::atomic<std::uint64_t> observed_{0};
    std::atomic<std::uint64_t> total_ns_{0};
    std::atomic<std::uint64_t> max_ns_{0};
};

struct RuntimeMetrics {
    LatencySamples websocket_callback_to_rtl_output;
    LatencySamples websocket_callback_to_paper_decision;
    LatencySamples rtl_cpp_rtl_roundtrip;
    LatencySamples modeled_singapore_total;
    LatencySamples modeled_tokyo_total;
    LatencySamples modeled_midpoint_total;
    LatencySamples modeled_singapore_network;
    LatencySamples modeled_tokyo_network;
    LatencySamples modeled_midpoint_network;
    LatencySamples websocket_interarrival_jitter;
    LatencySamples rtl_eval_wall_time;
    LatencySamples websocket_callback_to_strategy_enqueue;
    LatencySamples strategy_queue_wait;
    std::atomic<double> ws_interarrival_jitter_us{0.0};
    std::atomic<std::uint64_t> rtl_cpp_spread_mismatches{0};
};

struct RuntimeResourceSample {
    double elapsed_seconds = 0.0;
    double process_cpu_pct = std::numeric_limits<double>::quiet_NaN();
    double system_cpu_pct = std::numeric_limits<double>::quiet_NaN();
    double process_ram_mb = std::numeric_limits<double>::quiet_NaN();
    double system_ram_used_mb = std::numeric_limits<double>::quiet_NaN();
};

std::string summarize_resource_samples(
    const std::vector<RuntimeResourceSample>& samples,
    double RuntimeResourceSample::*member, const char* unit,
    const char* sum_unit) {
    std::vector<std::pair<double, double>> values;
    values.reserve(samples.size());
    for (const auto& sample : samples) {
        const double value = sample.*member;
        if (std::isfinite(value)) {
            values.emplace_back(sample.elapsed_seconds, value);
        }
    }
    if (values.empty()) return "no valid samples";

    std::vector<double> sorted;
    sorted.reserve(values.size());
    double sum = 0.0;
    double integral = 0.0;
    for (const auto& [elapsed, value] : values) {
        (void)elapsed;
        sorted.push_back(value);
        sum += value;
    }
    for (std::size_t i = 1; i < values.size(); ++i) {
        const double interval = values[i].first - values[i - 1].first;
        if (interval > 0.0) {
            integral += (values[i - 1].second + values[i].second) *
                        0.5 * interval;
        }
    }
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double fraction) {
        const auto index = static_cast<std::size_t>(
            std::floor((sorted.size() - 1) * fraction));
        return sorted[index];
    };
    std::ostringstream output;
    output << std::fixed << std::setprecision(3)
           << "samples=" << sorted.size()
           << " p05=" << percentile(0.05) << unit
           << " p50=" << percentile(0.50) << unit
           << " p95=" << percentile(0.95) << unit
           << " p99=" << percentile(0.99) << unit
           << " max=" << sorted.back() << unit
           << " sample_sum=" << sum << sum_unit
           << " time_integral=" << integral << unit << "-seconds";
    return output.str();
}

class ArrivalJitter {
public:
    double observe(std::chrono::steady_clock::time_point received_at) {
        if (last_ != std::chrono::steady_clock::time_point{}) {
            const double interval_us = std::chrono::duration<double,
                std::micro>(received_at - last_).count();
            if (has_interval_) {
                const double deviation = std::abs(interval_us - mean_interval_us_);
                mean_absolute_deviation_us_ =
                    0.95 * mean_absolute_deviation_us_ + 0.05 * deviation;
                mean_interval_us_ =
                    0.95 * mean_interval_us_ + 0.05 * interval_us;
            } else {
                mean_interval_us_ = interval_us;
                has_interval_ = true;
            }
        }
        last_ = received_at;
        return mean_absolute_deviation_us_;
    }

private:
    std::chrono::steady_clock::time_point last_{};
    double mean_interval_us_ = 0.0;
    double mean_absolute_deviation_us_ = 0.0;
    bool has_interval_ = false;
};

double process_ram_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#else
    std::ifstream statm("/proc/self/statm");
    long total_pages = 0, resident_pages = 0;
    if (statm >> total_pages >> resident_pages) {
        const long page_size = sysconf(_SC_PAGESIZE);
        return static_cast<double>(resident_pages * page_size) / (1024.0 * 1024.0);
    }
#endif
    return std::numeric_limits<double>::quiet_NaN();
}

double system_ram_used_mb() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<double>(status.ullTotalPhys - status.ullAvailPhys) /
               (1024.0 * 1024.0);
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string key, unit;
    double total_kb = 0, available_kb = 0, value = 0;
    while (meminfo >> key >> value >> unit) {
        if (key == "MemTotal:") total_kb = value;
        else if (key == "MemAvailable:") available_kb = value;
    }
    if (total_kb > 0) return (total_kb - available_kb) / 1024.0;
#endif
    return std::numeric_limits<double>::quiet_NaN();
}

double system_cpu_percent() {
#ifdef _WIN32
    static std::uint64_t previous_total = 0, previous_idle = 0;
    static bool initialized = false;
    FILETIME idle_time{}, kernel_time{}, user_time{};
    if (GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        const auto to_u64 = [](const FILETIME& time) {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) |
                   time.dwLowDateTime;
        };
        const auto idle = to_u64(idle_time);
        const auto total = to_u64(kernel_time) + to_u64(user_time);
        if (!initialized) {
            previous_total = total;
            previous_idle = idle;
            initialized = true;
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (total > previous_total) {
            const auto delta_total = total - previous_total;
            const auto delta_idle = idle - previous_idle;
            previous_total = total;
            previous_idle = idle;
            return 100.0 * (delta_total - delta_idle) / delta_total;
        }
        previous_total = total;
        previous_idle = idle;
    }
#else
    static std::uint64_t previous_total = 0, previous_idle = 0;
    static bool initialized = false;
    std::ifstream stats("/proc/stat");
    std::string label;
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0,
                  iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (stats >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
        const auto total = user + nice + system + idle + iowait + irq + softirq + steal;
        const auto idle_total = idle + iowait;
        if (!initialized) {
            previous_total = total;
            previous_idle = idle_total;
            initialized = true;
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (total > previous_total) {
            const auto delta_total = total - previous_total;
            const auto delta_idle = idle_total - previous_idle;
            previous_total = total;
            previous_idle = idle_total;
            return 100.0 * (delta_total - delta_idle) / delta_total;
        }
        previous_total = total;
        previous_idle = idle_total;
    }
#endif
    return std::numeric_limits<double>::quiet_NaN();
}

double process_cpu_percent() {
    static std::clock_t previous_cpu = 0;
    static std::chrono::steady_clock::time_point previous_wall{};
    static bool initialized = false;
    const auto now_cpu = std::clock();
    const auto now_wall = std::chrono::steady_clock::now();
    if (!initialized) {
        previous_cpu = now_cpu;
        previous_wall = now_wall;
        initialized = true;
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto wall_seconds = std::chrono::duration<double>(now_wall - previous_wall).count();
    const auto cpu_seconds = static_cast<double>(now_cpu - previous_cpu) / CLOCKS_PER_SEC;
    previous_cpu = now_cpu;
    previous_wall = now_wall;
    return wall_seconds > 0 ? cpu_seconds * 100.0 / wall_seconds
                            : std::numeric_limits<double>::quiet_NaN();
}

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
    std::uint16_t paper_slippage_bps = 0;
    std::uint64_t min_profit = 0;
    std::uint32_t max_quote_age_ms = 1'500;
    std::uint32_t duration_sec = 5;
    std::uint32_t synthetic_tick_ms = 10;
    double zscore_threshold = 2.0;
    bool verbose = false;
    bool latency_every_message = false;
    bool telemetry_enabled = true;
    std::uint64_t network_seed = 42;
    std::optional<std::uint32_t> market_cpu;
    std::optional<std::uint32_t> strategy_cpu;
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
        if (symbol.size() >= 32) {
            throw std::invalid_argument("symbols must be shorter than 32 bytes");
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
        throw std::invalid_argument("configure between 1 and 65534 symbols");
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
    if (const char* configured_symbols = std::getenv("INGESTION_SYMBOLS")) {
        options.symbols = parse_symbols(configured_symbols);
    }
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
        } else if (arg == "--paper-slippage-bps") {
            options.paper_slippage_bps = parse_fee(
                value("--paper-slippage-bps"), "--paper-slippage-bps");
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
        } else if (arg == "--duration-sec") {
            const auto duration = std::stoul(value("--duration-sec"));
            if (duration == 0 || duration > 86'400) {
                throw std::invalid_argument("--duration-sec must be 1-86400");
            }
            options.duration_sec = static_cast<std::uint32_t>(duration);
        } else if (arg == "--synthetic-tick-ms") {
            const auto interval = std::stoul(value("--synthetic-tick-ms"));
            if (interval == 0 || interval > 60'000) {
                throw std::invalid_argument(
                    "--synthetic-tick-ms must be 1-60000");
            }
            options.synthetic_tick_ms = static_cast<std::uint32_t>(interval);
        } else if (arg == "--zscore-threshold") {
            options.zscore_threshold = std::stod(value("--zscore-threshold"));
            if (!std::isfinite(options.zscore_threshold) ||
                options.zscore_threshold < 0.0) {
                throw std::invalid_argument(
                    "--zscore-threshold must be finite and nonnegative");
            }
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--latency-every-message") {
            options.latency_every_message = true;
        } else if (arg == "--network-seed") {
            options.network_seed = std::stoull(value("--network-seed"));
        } else if (arg == "--market-cpu" || arg == "--strategy-cpu") {
            const auto parsed = std::stoul(value(arg.c_str()));
            if (parsed > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("CPU index is too large");
            if (arg == "--market-cpu")
                options.market_cpu = static_cast<std::uint32_t>(parsed);
            else
                options.strategy_cpu = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--no-telemetry") {
            options.telemetry_enabled = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: market_data_engine [--source abides|websocket|synthetic] "
                   "[--symbols BTCUSDT,ETHUSDT] [--socket PATH] "
                   "[--bybit-fee-bps N] [--binance-fee-bps N] "
                   "[--paper-slippage-bps N] "
                   "[--min-profit PRICE] [--max-quote-age-ms N] "
                   "[--zscore-threshold N] [--duration-sec N] "
                   "[--synthetic-tick-ms N] [--network-seed N] "
                   "[--market-cpu N] [--strategy-cpu N] "
                   "[--latency-every-message] [--no-telemetry] [--verbose]\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }
    if (options.source != "abides" && options.source != "websocket" &&
        options.source != "synthetic") {
        throw std::invalid_argument(
            "--source must be abides, websocket, or synthetic");
    }
    return options;
}

struct Decision {
    std::uint16_t symbol_id;
    std::uint32_t transaction_id;
    bool candidate;
    std::uint8_t direction;
    std::int64_t net_profit;
    std::uint32_t bybit_bid;
    std::uint32_t bybit_ask;
    std::uint32_t binance_bid;
    std::uint32_t binance_ask;
    std::uint16_t bybit_fee_bps;
    std::uint16_t binance_fee_bps;
    std::uint16_t paper_slippage_bps;
    std::chrono::steady_clock::time_point received_at{};
    std::chrono::steady_clock::time_point enqueued_at{};
};

struct DecisionResponse {
    std::uint32_t transaction_id = 0;
    bool execute = false;
    std::uint32_t quantity = 0;
};

static_assert(std::is_trivially_copyable_v<Decision>,
              "mapped strategy messages must remain fixed-size POD records");

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
    std::uint16_t paper_slippage_bps;
};

class DecisionWorker {
public:
    DecisionWorker(const std::vector<std::string>& symbols, bool verbose,
                   double zscore_threshold, std::mutex& output_mutex,
                   RuntimeMetrics& runtime_metrics,
                   metrics::TimescaleTelemetry* telemetry,
                   SpscRing<DecisionResponse, kDecisionQueueCapacity>& responses,
                   std::optional<std::uint32_t> strategy_cpu)
        : symbols_(symbols), verbose_(verbose),
          zscore_threshold_(zscore_threshold), output_mutex_(output_mutex),
          metrics_(runtime_metrics), telemetry_(telemetry),
          responses_(responses), strategy_cpu_(strategy_cpu) {
        windows_.reserve(symbols.size());
        positions_.resize(symbols.size());
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            windows_.emplace_back();
        }
        thread_ = std::thread(&DecisionWorker::run, this);
    }

    ~DecisionWorker() {
        stop();
    }

    bool submit(Decision decision) {
        decision.enqueued_at = std::chrono::steady_clock::now();
        if (!queue_.try_push(std::move(decision))) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    void stop() {
        stopping_.store(true, std::memory_order_release);
        queue_.wake();
        if (thread_.joinable()) {
            thread_.join();
        }
        std::lock_guard<std::mutex> output_lock(output_mutex_);
        for (std::size_t i = 0; i < positions_.size(); ++i) {
            if (!positions_[i]) {
                continue;
            }
            const auto& position = *positions_[i];
            const auto closing_pnl = realized_pnl(
                position, position.mark_bybit_bid, position.mark_bybit_ask,
                position.mark_binance_bid, position.mark_binance_ask);
            cumulative_pnl_.fetch_add(closing_pnl, std::memory_order_relaxed);
            std::cout << "[TRADE] stopped out paper pair symbol="
                      << symbols_[i] << " direction="
                      << (position.direction == 1
                              ? "BUY_BYBIT_SELL_BINANCE"
                              : "BUY_BINANCE_SELL_BYBIT")
                      << " quantity=" << Config{}.position_size
                      << " realized_pnl=" << std::fixed
                      << std::setprecision(4)
                      << closing_pnl
                      << " reason=application_shutdown\n";
            positions_[i].reset();
        }
    }

    std::size_t dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }
    const std::string& shared_memory_name() const noexcept {
        return queue_.name();
    }
    std::uint64_t trades() const {
        return trades_.load(std::memory_order_relaxed);
    }
    double cumulative_pnl() const {
        return cumulative_pnl_.load(std::memory_order_relaxed);
    }
    std::uint64_t decision_count() const {
        return decision_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t candidate_decision_count() const {
        return candidate_decision_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t accepted_decision_count() const {
        return accepted_decision_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t warmup_rejection_count() const {
        return warmup_rejection_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t zscore_rejection_count() const {
        return zscore_rejection_count_.load(std::memory_order_relaxed);
    }
    std::uint64_t no_candidate_count() const {
        return no_candidate_count_.load(std::memory_order_relaxed);
    }

private:
    static double realized_pnl(const PaperPosition& position,
                               std::uint32_t bybit_bid,
                               std::uint32_t bybit_ask,
                               std::uint32_t binance_bid,
                               std::uint32_t binance_ask) {
        const auto bybit_fee =
            static_cast<double>(position.bybit_fee_bps +
                                position.paper_slippage_bps) / kFeeScale;
        const auto binance_fee =
            static_cast<double>(position.binance_fee_bps +
                                position.paper_slippage_bps) / kFeeScale;
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
               static_cast<double>(kPriceScale) * Config{}.position_size;
    }

    void run() {
        if (strategy_cpu_ && !pin_current_thread_to_cpu(*strategy_cpu_)) {
            std::lock_guard<std::mutex> output_lock(output_mutex_);
            std::cerr << "[AFFINITY] failed to pin strategy worker to CPU "
                      << *strategy_cpu_ << '\n';
        }
        while (true) {
            Decision decision;
            const auto observed = queue_.wake_sequence();
            if (!queue_.try_pop(decision)) {
                if (stopping_.load(std::memory_order_acquire)) return;
                if (!queue_.try_pop(decision)) {
                    queue_.wait_for_data(observed);
                    continue;
                }
            }
            metrics_.strategy_queue_wait.add(
                std::chrono::steady_clock::now() - decision.enqueued_at);

            if (decision.symbol_id >= symbols_.size()) {
                continue;
            }
            const auto symbol_index = static_cast<std::size_t>(decision.symbol_id);
            const auto& symbol_name = symbols_[symbol_index];
            const auto cpp_spread = strategy::calculate_net_spread(
                decision.bybit_bid, decision.bybit_ask,
                decision.binance_bid, decision.binance_ask,
                decision.bybit_fee_bps, decision.binance_fee_bps);
            if (cpp_spread.value != decision.net_profit ||
                (decision.candidate && cpp_spread.direction != decision.direction)) {
                metrics_.rtl_cpp_spread_mismatches.fetch_add(
                    1, std::memory_order_relaxed);
            }
            const double profit =
                static_cast<double>(cpp_spread.value) / 100'000'000.0;
            auto& window = windows_[symbol_index];
            const double zscore = window.score(profit);
            const bool warmup_complete = window.ready();
            const auto history_samples = static_cast<std::uint16_t>(window.size());
            window.push(profit);

            // RTL screens the executable, fee-adjusted cross-venue spread;
            // C++ applies the statistical gate to that same net spread.
            const auto risk = strategy::evaluate_risk(
                decision.candidate, warmup_complete, zscore,
                zscore_threshold_);
            const bool accepted = risk.accepted;
            decision_count_.fetch_add(1, std::memory_order_relaxed);
            if (decision.candidate) {
                candidate_decision_count_.fetch_add(1,
                                                    std::memory_order_relaxed);
            }
            if (accepted) {
                accepted_decision_count_.fetch_add(1,
                                                   std::memory_order_relaxed);
            } else if (!decision.candidate) {
                no_candidate_count_.fetch_add(1,
                                             std::memory_order_relaxed);
            } else if (!warmup_complete) {
                warmup_rejection_count_.fetch_add(1,
                                                   std::memory_order_relaxed);
            } else {
                zscore_rejection_count_.fetch_add(1,
                                                  std::memory_order_relaxed);
            }
            const auto decision_time_ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::system_clock::now()
                                               .time_since_epoch())
                                               .count();
            if (telemetry_) {
                telemetry_->publish_decision(
                    symbol_name.c_str(), decision_time_ms, decision.candidate,
                    accepted, decision.direction, history_samples,
                    decision.bybit_fee_bps, decision.binance_fee_bps,
                    decision.paper_slippage_bps, zscore,
                    static_cast<double>(cpp_spread.value) /
                        (kPriceScale * kFeeScale));
            }
            const bool show_decision =
                verbose_ || (!accepted && printed_rejects_ < 10);
            if (!accepted) {
                ++printed_rejects_;
            }

            const char* direction =
                decision.direction == 1 ? "BUY_BYBIT_SELL_BINANCE"
                : decision.direction == 2 ? "BUY_BINANCE_SELL_BYBIT"
                                           : "NONE";
            if (show_decision) {
                std::lock_guard<std::mutex> output_lock(output_mutex_);
                std::cout << "[DECISION] symbol=" << symbol_name
                          << " result=" << (accepted ? "ACCEPT" : "REJECT")
                          << " reason="
                          << risk.reason
                          << " zscore=" << zscore
                          << " zscore_threshold=" << zscore_threshold_;
                if (decision.candidate) {
                    std::cout << " direction=" << direction
                              << " spread="
                              << static_cast<double>(cpp_spread.value) /
                                     100'000'000.0;
                }
                if (accepted) {
                    std::cout << " quantity=" << Config{}.position_size;
                }
                std::cout << '\n';
            }

            auto& position = positions_[symbol_index];
            bool execute = false;
            if (position &&
                (!decision.candidate ||
                 (accepted && position->direction != decision.direction))) {
                const auto closing_pnl = realized_pnl(
                    *position, decision.bybit_bid, decision.bybit_ask,
                    decision.binance_bid, decision.binance_ask);
                cumulative_pnl_.fetch_add(closing_pnl,
                                          std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> output_lock(output_mutex_);
                    std::cout << "[TRADE] stopped out paper pair symbol="
                              << symbol_name
                              << " direction="
                              << (position->direction == 1
                                      ? "BUY_BYBIT_SELL_BINANCE"
                                      : "BUY_BINANCE_SELL_BYBIT")
                              << " quantity=" << Config{}.position_size
                              << " realized_pnl=" << std::fixed
                              << std::setprecision(4) << closing_pnl
                              << " reason="
                              << (decision.candidate ? "opportunity_reversed"
                                                     : "RTL_candidate_cleared")
                              << '\n';
                }
                position.reset();
            }

            if (accepted && !position) {
                execute = true;
                trades_.fetch_add(1, std::memory_order_relaxed);
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
                                         decision.binance_fee_bps,
                                         decision.paper_slippage_bps};
                {
                    std::lock_guard<std::mutex> output_lock(output_mutex_);
                    std::cout << "[TRADE] entered paper pair symbol="
                              << symbol_name << " direction=" << direction
                              << " quantity=" << Config{}.position_size
                              << " buy_price=" << std::fixed
                              << std::setprecision(4)
                              << (decision.direction == 1
                                      ? decision.bybit_ask
                                      : decision.binance_ask) /
                                     static_cast<double>(kPriceScale)
                              << " sell_price="
                              << (decision.direction == 1
                                      ? decision.binance_bid
                                      : decision.bybit_bid) /
                                     static_cast<double>(kPriceScale)
                              << " estimated_net_profit="
                          << static_cast<double>(cpp_spread.value) /
                                     (kPriceScale * kFeeScale)
                              << '\n';
                }
            }
            if (accepted && position) {
                position->mark_bybit_bid = decision.bybit_bid;
                position->mark_bybit_ask = decision.bybit_ask;
                position->mark_binance_bid = decision.binance_bid;
                position->mark_binance_ask = decision.binance_ask;
            }
            metrics_.websocket_callback_to_paper_decision.add(
                std::chrono::steady_clock::now() - decision.received_at);
            const auto quantity = execute
                ? static_cast<std::uint32_t>(Config{}.position_size)
                : 0U;
            if (!responses_.try_push(
                    {decision.transaction_id, execute, quantity})) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

private:
    std::vector<strategy::RollingZScore> windows_;
    std::vector<std::optional<PaperPosition>> positions_;
    std::vector<std::string> symbols_;
    bool verbose_;
    double zscore_threshold_;
    std::mutex& output_mutex_;
    RuntimeMetrics& metrics_;
    metrics::TimescaleTelemetry* telemetry_;
    SpscRing<DecisionResponse, kDecisionQueueCapacity>& responses_;
    std::optional<std::uint32_t> strategy_cpu_;
    MmapSpscRing<Decision, kDecisionQueueCapacity> queue_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::uint64_t> trades_{0};
    std::atomic<double> cumulative_pnl_{0.0};
    std::atomic<std::uint64_t> decision_count_{0};
    std::atomic<std::uint64_t> candidate_decision_count_{0};
    std::atomic<std::uint64_t> accepted_decision_count_{0};
    std::atomic<std::uint64_t> warmup_rejection_count_{0};
    std::atomic<std::uint64_t> zscore_rejection_count_{0};
    std::atomic<std::uint64_t> no_candidate_count_{0};
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
        std::cerr << std::unitbuf;
        const Options options = parse_options(argc, argv);
        std::unique_ptr<metrics::TimescaleTelemetry> telemetry;
        if (options.telemetry_enabled) {
            const char* connection_env = std::getenv("TIMESCALE_CONN");
            telemetry = std::make_unique<metrics::TimescaleTelemetry>(
                connection_env ? connection_env :
                    "host=localhost port=5433 dbname=market_data user=market password=market");
        }
        std::mutex output_mutex;
        std::unordered_map<std::string, std::size_t, TransparentStringHash,
                           std::equal_to<>> symbol_ids;
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
                  << " binance_fee_bps=" << options.binance_fee_bps
                  << " paper_slippage_bps=" << options.paper_slippage_bps
                  << " zscore_threshold=" << options.zscore_threshold << '\n';

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
        rtl->decision_valid_i = 0;
        rtl->decision_execute_i = 0;
        rtl->decision_quantity_i = 0;
        rtl->decision_transaction_id_i = 0;
        rtl->eval();
        rtl->clk = 1;
        rtl->eval();
        rtl->clk = 0;
        rtl->rst_n = 1;
        rtl->eval();

        RuntimeMetrics runtime_metrics;
        SpscRing<DecisionResponse, kDecisionQueueCapacity> decision_responses;
        DecisionWorker decision_worker(options.symbols, options.verbose,
                                       options.zscore_threshold, output_mutex,
                                       runtime_metrics, telemetry.get(),
                                       decision_responses,
                                       options.strategy_cpu);
        std::cout << "[DMA_SIM] named_shared_memory="
                  << decision_worker.shared_memory_name()
                  << " capacity=" << kDecisionQueueCapacity
                  << " C++ independently recomputes net spread from copied prices\n";

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
        } else if (options.source == "websocket") {
            websocket = std::make_unique<WebSocketFeed>(options.symbols);
            websocket->start();
            std::cout << "[SOURCE] connecting to public Bybit and Binance "
                         "WebSockets\n";
        } else {
            std::cout << "[SOURCE] deterministic synthetic cross-venue feed\n";
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
        std::vector<std::array<ArrivalJitter, 2>> arrival_jitter(
            options.symbols.size());
        latency_model::Model network_model(options.network_seed);
        std::cout << "[ROUTE_MODEL] network_seed=" << options.network_seed
                  << " fiber_route_stretch=1.35 refractive_index=1.468"
                  << " Bybit_endpoint=Singapore Binance_endpoint=Tokyo_assumption"
                  << " SG_firm_max_rtt_floor_us="
                  << std::max(
                         latency_model::Model::round_trip_fiber_floor_ns(
                             latency_model::FirmLocation::Singapore,
                             latency_model::VenueEndpoint::BybitSingapore),
                         latency_model::Model::round_trip_fiber_floor_ns(
                             latency_model::FirmLocation::Singapore,
                             latency_model::VenueEndpoint::BinanceTokyo)) /
                         1000.0
                  << " Tokyo_firm_max_rtt_floor_us="
                  << std::max(
                         latency_model::Model::round_trip_fiber_floor_ns(
                             latency_model::FirmLocation::Tokyo,
                             latency_model::VenueEndpoint::BybitSingapore),
                         latency_model::Model::round_trip_fiber_floor_ns(
                             latency_model::FirmLocation::Tokyo,
                             latency_model::VenueEndpoint::BinanceTokyo)) /
                         1000.0
                  << " midpoint_max_rtt_floor_us="
                  << std::max(
                         latency_model::Model::round_trip_fiber_floor_ns(
                             latency_model::FirmLocation::Midpoint,
                             latency_model::VenueEndpoint::BybitSingapore),
                         latency_model::Model::round_trip_fiber_floor_ns(
                             latency_model::FirmLocation::Midpoint,
                             latency_model::VenueEndpoint::BinanceTokyo)) /
                         1000.0 << '\n';
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
        struct PendingDecision {
            bool active = false;
            std::uint32_t transaction_id = 0;
            std::size_t symbol_index = 0;
            std::chrono::steady_clock::time_point rtl_input_received_at{};
            double ws_interarrival_jitter_us = 0.0;
        };
        std::vector<PendingDecision> pending_decisions(65'536);
        std::size_t pending_decision_count = 0;
        const auto process_decision_responses = [&] {
            DecisionResponse response;
            while (decision_responses.try_pop(response)) {
                auto& pending = pending_decisions[
                    response.transaction_id % pending_decisions.size()];
                if (!pending.active ||
                    pending.transaction_id != response.transaction_id) {
                    throw std::runtime_error(
                        "C++ returned an unknown or overwritten transaction");
                }
                rtl->clk = 0;
                rtl->input_valid = 0;
                rtl->decision_valid_i = 1;
                rtl->decision_execute_i = response.execute;
                rtl->decision_quantity_i = response.quantity;
                rtl->decision_transaction_id_i = response.transaction_id;
                rtl->eval();
                rtl->clk = 1;
                rtl->eval();
                const auto rtl_receive_at = std::chrono::steady_clock::now();
                if (!rtl->decision_ack_valid_o ||
                    rtl->decision_transaction_id_o != response.transaction_id ||
                    static_cast<bool>(rtl->decision_executed_o) != response.execute ||
                    rtl->decision_quantity_o != response.quantity) {
                    throw std::runtime_error(
                        "Verilated C++ decision acknowledgement mismatch");
                }
                const auto roundtrip = rtl_receive_at -
                                       pending.rtl_input_received_at;
                runtime_metrics.rtl_cpp_rtl_roundtrip.add(roundtrip);
                const auto input_received_steady_ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        pending.rtl_input_received_at.time_since_epoch()).count();
                const auto output_sent_steady_ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        rtl_receive_at.time_since_epoch()).count();
                const double roundtrip_ns = std::chrono::duration<double,
                    std::nano>(roundtrip).count();
                const auto modeled = network_model.sample(
                    roundtrip_ns, pending.ws_interarrival_jitter_us);
                runtime_metrics.modeled_singapore_network.add(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(
                        std::max(0.0, modeled.singapore_total_ns - roundtrip_ns))));
                runtime_metrics.modeled_tokyo_network.add(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(
                        std::max(0.0, modeled.tokyo_total_ns - roundtrip_ns))));
                runtime_metrics.modeled_midpoint_network.add(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(
                        std::max(0.0, modeled.midpoint_total_ns - roundtrip_ns))));
                runtime_metrics.modeled_singapore_total.add(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(
                        modeled.singapore_total_ns)));
                runtime_metrics.modeled_tokyo_total.add(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(
                        modeled.tokyo_total_ns)));
                runtime_metrics.modeled_midpoint_total.add(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(
                        modeled.midpoint_total_ns)));
                runtime_metrics.ws_interarrival_jitter_us.store(
                    pending.ws_interarrival_jitter_us,
                    std::memory_order_relaxed);
                if (telemetry) {
                    telemetry->publish_roundtrip(
                        options.symbols[pending.symbol_index].c_str(),
                        wall_now_ms(), response.transaction_id,
                        output_sent_steady_ns, input_received_steady_ns,
                        roundtrip_ns,
                        pending.ws_interarrival_jitter_us,
                        modeled.singapore_total_ns, modeled.tokyo_total_ns,
                        modeled.midpoint_total_ns, modeled.tail_spike_ns);
                }
                if (options.latency_every_message) {
                    std::lock_guard<std::mutex> output_lock(output_mutex);
                    std::cout << "[LATENCY] transaction_id="
                              << response.transaction_id
                              << " symbol="
                              << options.symbols[pending.symbol_index]
                              << " t_input_received_steady_ns="
                              << input_received_steady_ns
                              << " t_output_sent_steady_ns="
                              << output_sent_steady_ns
                              << " verilog_cpp_verilog_roundtrip_us=" << std::fixed
                              << std::setprecision(3) << roundtrip_ns / 1000.0
                              << " websocket_interarrival_jitter_us="
                              << pending.ws_interarrival_jitter_us
                              << " estimated_total_singapore_us="
                              << modeled.singapore_total_ns / 1000.0
                              << " estimated_total_tokyo_us="
                              << modeled.tokyo_total_ns / 1000.0
                              << " estimated_total_midpoint_us="
                              << modeled.midpoint_total_ns / 1000.0
                              << " pareto_tail_spike_us="
                              << modeled.tail_spike_ns / 1000.0 << '\n';
                    std::cout.flush();
                }
                pending.active = false;
                --pending_decision_count;
                rtl->clk = 0;
                rtl->decision_valid_i = 0;
                rtl->eval();
            }
        };
        const auto feed_rtl = [&](std::size_t symbol_index,
                                  const VenueQuote& bybit_quote,
                                  const VenueQuote& binance_quote,
                                  bool quotes_valid,
                                  std::chrono::steady_clock::time_point
                                      message_received_at,
                                  double ws_interarrival_jitter_us) {
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
            // The RTL captures inputs on this edge and publishes the matching
            // spread output on the following rising edge.
            rtl->clk = 0;
            rtl->input_valid = 0;
            rtl->eval();
            rtl->clk = 1;
            rtl->eval();
            const auto rtl_completed_at = std::chrono::steady_clock::now();
            if (rtl->transaction_id_o != transaction_id) {
                throw std::runtime_error(
                    "Verilated spread output transaction ID mismatch");
            }
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
            if (options.verbose || updates <= 10) {
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
            if (options.verbose && new_candidate) {
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
            rtl->clk = 0;
            rtl->input_valid = 0;
            rtl->eval();
            if (!decision_worker.submit(
                {static_cast<std::uint16_t>(symbol_index), transaction_id, candidate,
                 static_cast<std::uint8_t>(rtl->direction_o), profit,
                 bybit_quote.bid, bybit_quote.ask, binance_quote.bid,
                 binance_quote.ask, options.bybit_fee_bps,
                 options.binance_fee_bps, options.paper_slippage_bps,
                 message_received_at})) {
                // Saturation is a counted fail-closed rejection. Do not block
                // the market loop waiting for ring capacity.
                rtl->decision_valid_i = 1;
                rtl->decision_execute_i = 0;
                rtl->decision_quantity_i = 0;
                rtl->decision_transaction_id_i = transaction_id;
                rtl->clk = 1;
                rtl->eval();
                if (!rtl->decision_ack_valid_o ||
                    rtl->decision_transaction_id_o != transaction_id ||
                    rtl->decision_executed_o || rtl->decision_quantity_o != 0) {
                    throw std::runtime_error(
                        "RTL rejected-overflow acknowledgement mismatch");
                }
                rtl->clk = 0;
                rtl->decision_valid_i = 0;
                rtl->eval();
                return;
            }
            runtime_metrics.websocket_callback_to_strategy_enqueue.add(
                std::chrono::steady_clock::now() - message_received_at);
            auto& pending = pending_decisions[
                transaction_id % pending_decisions.size()];
            if (pending.active) {
                throw std::runtime_error(
                    "too many pending C++ decisions for fixed tracking table");
            }
            pending = {true, transaction_id, symbol_index, rtl_started_at,
                       ws_interarrival_jitter_us};
            ++pending_decision_count;
            process_decision_responses();
        };
        const auto accept_market_update = [&](const QuoteUpdate& update) {
            const auto symbol =
                symbol_ids.find(std::string_view(update.symbol.data()));
            if (symbol == symbol_ids.end()) {
                return;
            }
            auto& pair = pairs[symbol->second];
            if (telemetry) {
                telemetry->publish_price(
                    update.symbol.data(),
                    update.venue == Venue::Bybit ? "bybit" : "binance",
                    update.bid, update.ask, wall_now_ms(),
                    update.exchange_time_ms);
            }
            auto& quote =
                update.venue == Venue::Bybit ? pair.bybit : pair.binance;
            auto& jitter = arrival_jitter[symbol->second][
                update.venue == Venue::Bybit ? 0 : 1];
            const auto observed_jitter_us = jitter.observe(update.received_at);
            runtime_metrics.websocket_interarrival_jitter.add(
                std::chrono::nanoseconds(static_cast<std::int64_t>(
                    std::max(0.0, observed_jitter_us) * 1000.0)));
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
                     update.received_at, observed_jitter_us);
        };

        std::cout << "[SOURCE] mode=" << options.source << " ready\n";
        const auto engine_started_at = std::chrono::steady_clock::now();
        std::vector<RuntimeResourceSample> resource_samples;
        resource_samples.reserve(static_cast<std::size_t>(
            options.duration_sec) + 2);
        auto last_status = std::chrono::steady_clock::now();
        std::atomic<bool> metrics_running{true};
        std::thread metrics_thread;
        metrics_thread = std::thread([&] {
            (void)process_cpu_percent();
            (void)system_cpu_percent();
            auto next_sample = std::chrono::steady_clock::now();
            while (metrics_running.load(std::memory_order_acquire)) {
                next_sample += std::chrono::seconds(1);
                std::this_thread::sleep_until(next_sample);
                if (!metrics_running.load(std::memory_order_acquire)) break;
                const auto sampled_at_ms = wall_now_ms();
                const double process_cpu = process_cpu_percent();
                const double system_cpu = system_cpu_percent();
                const double process_ram = process_ram_mb();
                const double system_ram = system_ram_used_mb();
                resource_samples.push_back({
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - engine_started_at)
                        .count(),
                    process_cpu, system_cpu, process_ram, system_ram});
                if (telemetry) {
                    telemetry->publish_metrics(
                        sampled_at_ms, process_cpu, system_cpu,
                        process_ram, system_ram,
                        runtime_metrics.websocket_callback_to_paper_decision
                            .percentile(0.50),
                        runtime_metrics.websocket_callback_to_paper_decision
                            .percentile(0.99),
                        runtime_metrics.rtl_cpp_rtl_roundtrip.percentile(0.05),
                        runtime_metrics.rtl_cpp_rtl_roundtrip.percentile(0.50),
                        runtime_metrics.rtl_cpp_rtl_roundtrip.percentile(0.95),
                        runtime_metrics.rtl_cpp_rtl_roundtrip.percentile(0.99),
                        runtime_metrics.rtl_cpp_rtl_roundtrip.maximum(),
                        runtime_metrics.modeled_singapore_total.percentile(0.50),
                        runtime_metrics.modeled_tokyo_total.percentile(0.50),
                        runtime_metrics.modeled_midpoint_total.percentile(0.50),
                        runtime_metrics.modeled_singapore_total.percentile(0.99),
                        runtime_metrics.modeled_tokyo_total.percentile(0.99),
                        runtime_metrics.modeled_midpoint_total.percentile(0.99),
                        runtime_metrics.ws_interarrival_jitter_us.load(
                            std::memory_order_relaxed),
                        decision_worker.trades(),
                        decision_worker.cumulative_pnl());
                }
                {
                    std::lock_guard<std::mutex> output_lock(output_mutex);
                    std::cout << "[SYSTEM] time_ms=" << sampled_at_ms
                              << " process_cpu_pct=" << process_cpu
                              << " system_cpu_pct=" << system_cpu
                              << " process_ram_mb=" << process_ram
                              << " system_ram_used_mb=" << system_ram
                              << " cpp_to_rtl_p50_us="
                              << runtime_metrics.rtl_cpp_rtl_roundtrip
                                     .percentile(0.50) / 1000.0
                              << " cpp_to_rtl_p99_us="
                              << runtime_metrics.rtl_cpp_rtl_roundtrip
                                     .percentile(0.99) / 1000.0
                              << " modeled_sg_p50_us="
                              << runtime_metrics.modeled_singapore_total
                                     .percentile(0.50) / 1000.0
                              << " modeled_tokyo_p50_us="
                              << runtime_metrics.modeled_tokyo_total
                                     .percentile(0.50) / 1000.0
                              << " modeled_midpoint_p50_us="
                              << runtime_metrics.modeled_midpoint_total
                                     .percentile(0.50) / 1000.0
                              << " modeled_sg_p99_us="
                              << runtime_metrics.modeled_singapore_total
                                     .percentile(0.99) / 1000.0
                              << " modeled_tokyo_p99_us="
                              << runtime_metrics.modeled_tokyo_total
                                     .percentile(0.99) / 1000.0
                              << " modeled_midpoint_p99_us="
                              << runtime_metrics.modeled_midpoint_total
                                     .percentile(0.99) / 1000.0
                              << '\n';
                    std::cout.flush();
                }
            }
        });
        if (options.market_cpu) {
            if (pin_current_thread_to_cpu(*options.market_cpu))
                std::cout << "[AFFINITY] market loop pinned to CPU "
                          << *options.market_cpu << '\n';
            else
                std::cerr << "[AFFINITY] failed to pin market loop to CPU "
                          << *options.market_cpu << '\n';
        }
        std::uint64_t synthetic_round = 0;
        while (running.load(std::memory_order_relaxed)) {
            process_decision_responses();
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
                                 simulated.received_at, 0.0);
                    }
                } catch (const std::exception& error) {
                    ++malformed_abides;
                    if (options.verbose || malformed_abides <= 10) {
                        std::cerr << "[SOURCE] malformed ABIDES packet: "
                                  << error.what() << '\n';
                    }
                }
            } else if (options.source == "websocket") {
                QuoteUpdate update{};
                if (websocket->wait_for_update(update,
                                               std::chrono::milliseconds(1))) {
                    accept_market_update(update);
                }
            } else {
                const auto round = synthetic_round++;
                const auto event_time_ms = wall_now_ms();
                const auto received_at = std::chrono::steady_clock::now();
                const std::int32_t phase = static_cast<std::int32_t>(round % 64);
                const std::int32_t edge = phase < 4 ? 3'000
                    : phase < 32 ? 100
                    : phase < 48 ? -100
                                 : 100;
                for (std::size_t i = 0; i < options.symbols.size(); ++i) {
                    const auto base = static_cast<std::uint32_t>(
                        100 * kPriceScale + i * 5 * kPriceScale);
                    const VenueQuote bybit_quote{
                        base, static_cast<std::uint32_t>(base + 100),
                        event_time_ms, received_at};
                    const auto binance_bid = static_cast<std::uint32_t>(
                        static_cast<std::int64_t>(base) + 100 + edge);
                    const VenueQuote binance_quote{
                        binance_bid, static_cast<std::uint32_t>(binance_bid + 100),
                        event_time_ms, received_at};
                    if (telemetry) {
                        telemetry->publish_price(
                            options.symbols[i].c_str(), "synthetic_bybit",
                            bybit_quote.bid, bybit_quote.ask, event_time_ms,
                            event_time_ms);
                        telemetry->publish_price(
                            options.symbols[i].c_str(), "synthetic_binance",
                            binance_quote.bid, binance_quote.ask, event_time_ms,
                            event_time_ms);
                    }
                    feed_rtl(i, bybit_quote, binance_quote, true, received_at,
                             50.0);
                }
                if (std::chrono::steady_clock::now() - engine_started_at >=
                    std::chrono::seconds(options.duration_sec)) {
                    running.store(false, std::memory_order_relaxed);
                }
                // Keep the synthetic market cadence independent from the
                // strategy roundtrip. Poll C++ replies during the idle part
                // of each tick instead of making every decision wait for the
                // full synthetic feed interval before RTL can acknowledge it.
                const auto next_synthetic_tick =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(options.synthetic_tick_ms);
                while (running.load(std::memory_order_relaxed) &&
                       std::chrono::steady_clock::now() <
                           next_synthetic_tick) {
                    process_decision_responses();
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(100));
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
                std::cout << " telemetry_queue_dropped="
                          << (telemetry ? telemetry->dropped() : 0)
                          << " " << benchmark.format();
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
        process_decision_responses();
        if (pending_decision_count != 0) {
            throw std::runtime_error("unacknowledged C++ decisions at shutdown");
        }
        if (runtime_metrics.rtl_cpp_spread_mismatches.load(
                std::memory_order_relaxed) != 0) {
            throw std::runtime_error(
                "C++ spread recomputation disagreed with RTL output");
        }
        metrics_running.store(false, std::memory_order_release);
        if (metrics_thread.joinable()) metrics_thread.join();
        if (telemetry) telemetry->publish_metrics(
            wall_now_ms(), process_cpu_percent(), system_cpu_percent(),
            process_ram_mb(), system_ram_used_mb(),
            runtime_metrics.websocket_callback_to_paper_decision.percentile(0.50),
            runtime_metrics.websocket_callback_to_paper_decision.percentile(0.99),
            runtime_metrics.rtl_cpp_rtl_roundtrip.percentile(0.05),
            runtime_metrics.rtl_cpp_rtl_roundtrip.percentile(0.50),
            runtime_metrics.rtl_cpp_rtl_roundtrip.percentile(0.95),
            runtime_metrics.rtl_cpp_rtl_roundtrip.percentile(0.99),
            runtime_metrics.rtl_cpp_rtl_roundtrip.maximum(),
            runtime_metrics.modeled_singapore_total.percentile(0.50),
            runtime_metrics.modeled_tokyo_total.percentile(0.50),
            runtime_metrics.modeled_midpoint_total.percentile(0.50),
            runtime_metrics.modeled_singapore_total.percentile(0.99),
            runtime_metrics.modeled_tokyo_total.percentile(0.99),
            runtime_metrics.modeled_midpoint_total.percentile(0.99),
            runtime_metrics.ws_interarrival_jitter_us.load(
                std::memory_order_relaxed),
            decision_worker.trades(), decision_worker.cumulative_pnl());
        if (telemetry) telemetry->stop();
        if (abides) {
            abides->close();
        }
        const double runtime_seconds = std::chrono::duration<double>(
                                           engine_finished_at -
                                           engine_started_at)
                                           .count();
        const RuntimeResourceSample latest_resource = resource_samples.empty()
            ? RuntimeResourceSample{}
            : resource_samples.back();
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
                           << "\nTelemetry queue drops: "
                           << (telemetry ? telemetry->dropped() : 0)
                           << "\nProcess CPU percent of one logical core: "
                           << latest_resource.process_cpu_pct
                           << "\nSystem CPU percent: "
                           << latest_resource.system_cpu_pct
                           << "\nProcess RAM MB: "
                           << latest_resource.process_ram_mb
                           << "\nSystem RAM used MB: "
                           << latest_resource.system_ram_used_mb
                           << "\nPaper trades: " << decision_worker.trades()
                           << "\nCumulative realized paper PnL: "
                           << decision_worker.cumulative_pnl()
                           << "\n\nPaper decision counts:\n"
                           << "all=" << decision_worker.decision_count()
                           << " rtl_candidates="
                           << decision_worker.candidate_decision_count()
                           << " accepted="
                           << decision_worker.accepted_decision_count()
                           << " rejected_warmup="
                           << decision_worker.warmup_rejection_count()
                           << " rejected_zscore="
                           << decision_worker.zscore_rejection_count()
                           << " rejected_no_rtl_candidate="
                           << decision_worker.no_candidate_count()
                           << "\n\nMeasured software latency (steady_clock, ns):\n"
                           << "WebSocket callback to Verilator output: "
                           << runtime_metrics.websocket_callback_to_rtl_output
                                  .summary()
                           << "\nWebSocket callback to C++ paper decision: "
                           << runtime_metrics
                                  .websocket_callback_to_paper_decision
                                  .summary()
                           << "\nVerilator eval wall time: "
                           << runtime_metrics.rtl_eval_wall_time.summary()
                           << "\nWebSocket callback to strategy enqueue: "
                           << runtime_metrics
                                  .websocket_callback_to_strategy_enqueue
                                  .summary()
                           << "\nStrategy queue wait: "
                           << runtime_metrics.strategy_queue_wait.summary()
                           << "\nRTL-to-C++-to-RTL acknowledgement: "
                           << runtime_metrics.rtl_cpp_rtl_roundtrip.summary()
                           << "\n\nModeled network-only round-trip latency (ns; no software component):\n"
                           << "Singapore firm route: "
                           << runtime_metrics.modeled_singapore_network.summary()
                           << "\nTokyo firm route: "
                           << runtime_metrics.modeled_tokyo_network.summary()
                           << "\nMidpoint firm route: "
                           << runtime_metrics.modeled_midpoint_network.summary()
                           << "\nObserved WebSocket interarrival-jitter proxy (ns): "
                           << runtime_metrics.websocket_interarrival_jitter.summary()
                           << "\nModeled Singapore firm total p50/p99 ns: "
                           << runtime_metrics.modeled_singapore_total.percentile(0.50)
                           << "/" << runtime_metrics.modeled_singapore_total.percentile(0.99)
                           << "\nModeled Tokyo firm total p50/p99 ns: "
                           << runtime_metrics.modeled_tokyo_total.percentile(0.50)
                           << "/" << runtime_metrics.modeled_tokyo_total.percentile(0.99)
                           << "\nModeled midpoint firm total p50/p99 ns: "
                           << runtime_metrics.modeled_midpoint_total.percentile(0.50)
                           << "/" << runtime_metrics.modeled_midpoint_total.percentile(0.99)
                           << "\nObserved WebSocket interarrival jitter proxy us: "
                           << runtime_metrics.ws_interarrival_jitter_us.load(
                                  std::memory_order_relaxed)
                           << "\n\nResource usage percentiles (one-second samples):\n"
                           << "Process CPU (% of one logical core): "
                           << summarize_resource_samples(
                                  resource_samples,
                                  &RuntimeResourceSample::process_cpu_pct,
                                  "%", "%-samples")
                           << "\nSystem CPU (% of all logical cores): "
                           << summarize_resource_samples(
                                  resource_samples,
                                  &RuntimeResourceSample::system_cpu_pct,
                                  "%", "%-samples")
                           << "\nProcess resident RAM: "
                           << summarize_resource_samples(
                                  resource_samples,
                                  &RuntimeResourceSample::process_ram_mb,
                                  " MB", " MB-samples")
                           << "\nSystem RAM used: "
                           << summarize_resource_samples(
                                  resource_samples,
                                  &RuntimeResourceSample::system_ram_used_mb,
                                  " MB", " MB-samples")
                           << "\n\nHardware and network evidence:\n"
                           << "RTL registered latency: 1 rising-edge cycle in the Verilated model; no physical clock period measured\n"
                           << "FPGA target / bitstream: not configured\n"
                           << "Post-place-and-route Fmax: not measured\n"
                           << "WNS / WHS timing closure: not measured\n"
                           << "FPGA LUT / FF / carry / BRAM / URAM / DSP usage: not measured for a target device\n"
                           << "Fiber network latency: modeled from city coordinates, route stretch, refractive index, OU noise, and Pareto spikes; not measured on a physical route\n"
                           << "Observed WebSocket receive timing: message interarrival deviation proxy; not one-way transport latency\n"
                           << "Order dispatch latency: not measured; this executable emits paper decisions and sends no exchange orders\n"
                           << "Mapped-memory transport: named OS shared-memory ring is used as an in-process DMA stand-in; no PCIe device or hardware DMA is present\n";
            std::cout << "[BENCHMARK] report=market_data_engine_benchmark.txt\n";
        } else {
            std::cerr << "[WARN] cannot create market_data_engine_benchmark.txt\n";
        }
        std::cout << "[DECISION_SUMMARY] total="
                  << decision_worker.decision_count()
                  << " rtl_candidates="
                  << decision_worker.candidate_decision_count()
                  << " accepted="
                  << decision_worker.accepted_decision_count()
                  << " rejected_warmup="
                  << decision_worker.warmup_rejection_count()
                  << " rejected_zscore="
                  << decision_worker.zscore_rejection_count()
                  << " rejected_no_rtl_candidate="
                  << decision_worker.no_candidate_count()
                  << " paper_entries=" << decision_worker.trades() << '\n';
        std::cout << "[STOP] updates=" << updates
                  << " candidates=" << candidates
                  << " stale_quotes=" << stale_quotes
                  << " pending_decisions=" << pending_decision_count
                  << " spread_mismatches="
                  << runtime_metrics.rtl_cpp_spread_mismatches.load(
                         std::memory_order_relaxed)
                  << " decision_queue_dropped=" << decision_worker.dropped()
                  << " rtl_cpp_rtl_roundtrip_samples="
                  << runtime_metrics.rtl_cpp_rtl_roundtrip.count();
        benchmark.total_updates = updates;
        benchmark.total_candidates = candidates;
        benchmark.stale_quotes = stale_quotes;
        benchmark.malformed_packets = malformed_abides;
        benchmark.decision_queue_dropped = decision_worker.dropped();
        if (websocket) {
            benchmark.websocket_dropped = websocket->dropped_updates();
            benchmark.websocket_malformed = websocket->malformed_messages();
        }
        std::cout << " telemetry_queue_dropped="
                  << (telemetry ? telemetry->dropped() : 0)
                  << " " << benchmark.format();
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
