#include "config.h"
#include "socket_reader.h"
#include "terminal_plot.h"
#include "verilator_hooked.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <deque>

#ifdef _WIN32
#include <signal.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int /*sig*/) {
    g_running.store(false);
}

// ---------------------------------------------------------------------------
// Rolling window for z-score computation
// ---------------------------------------------------------------------------
template <typename T>
class RollingWindow {
public:
    explicit RollingWindow(size_t capacity) : _capacity(capacity) {}

    void push_back(T value) {
        _data.push_back(value);
        if (_data.size() > _capacity) {
            _data.pop_front();
        }
    }

    size_t size() const { return _data.size(); }

    double mean() const {
        if (_data.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& v : _data) {
            sum += v;
        }
        return sum / static_cast<double>(_data.size());
    }

    double stddev() const {
        if (_data.size() < 2) return 0.0;
        double m = mean();
        double sq_sum = 0.0;
        for (const auto& v : _data) {
            double diff = v - m;
            sq_sum += diff * diff;
        }
        return std::sqrt(sq_sum / static_cast<double>(_data.size() - 1));
    }

    // z = (current - mean) / std
    double zscore(T current) const {
        double m = mean();
        double s = stddev();
        if (s == 0.0) return 0.0;
        return static_cast<double>(current - m) / s;
    }

private:
    size_t _capacity;
    std::deque<T> _data;
};

// ---------------------------------------------------------------------------
// Simple mean-reversion arbitrage strategy using z-score
// ---------------------------------------------------------------------------
struct ArbitrageSignal {
    int symbol_idx = 0;
    bool long_signal = false;    // z < -threshold
    bool short_signal = false;   // z > +threshold
    bool close_signal = false;   // was in position, z returned to 0
    double zscore = 0.0;
    double entry_price = 0.0;    // scaled units (1e8)
    double exit_price = 0.0;
    bool position_open = false;
};

class SimpleArbitrageStrategy {
public:
    explicit SimpleArbitrageStrategy(size_t num_symbols, double zscore_threshold = 2.0)
        : _num_symbols(num_symbols), _zscore_threshold(zscore_threshold) {
        _windows.resize(num_symbols, RollingWindow<double>(50)); // 50-sample rolling window
        _signals.resize(num_symbols);
        _positions.resize(num_symbols);
    }

    void update(double zscore, int symbol_idx) {
        _windows[symbol_idx].push_back(zscore);
        double current_z = _windows[symbol_idx].zscore(zscore);

        auto& signal = _signals[symbol_idx];
        signal.zscore = current_z;
        signal.symbol_idx = symbol_idx;

        // Check for new signals
        double z = current_z;
        if (!_positions[symbol_idx].position_open) {
            if (z < -_zscore_threshold) {
                signal.long_signal = true;       // Price is too low -> expect mean reversion
            } else if (z > _zscore_threshold) {
                signal.short_signal = true;      // Price is too high -> expect mean reversion
            }
        } else {
            // In a position, check if we should close
            if (z > 0.0 && signal.long_signal) {
                signal.close_signal = true;      // Long -> close when z goes positive
            } else if (z < 0.0 && signal.short_signal) {
                signal.close_signal = true;      // Short -> close when z goes negative
            }
        }
    }

    const ArbitrageSignal& signal(int idx) const {
        return _signals[idx];
    }

    void open_position(const ArbitrageSignal& sig, double price) {
        _positions[sig.symbol_idx].position_open = true;
        _positions[sig.symbol_idx].entry_price = price;
        _positions[sig.symbol_idx].is_long = sig.long_signal;
        _positions[sig.symbol_idx].size = 1.0;
    }

    void close_position(int idx, double price) {
        auto& pos = _positions[idx];
        if (!pos.position_open) return;

        double pnl = 0.0;
        if (pos.is_long) {
            pnl = (price - pos.entry_price) * pos.size;
        } else {
            pnl = (pos.entry_price - price) * pos.size;
        }
        pos.pnl += pnl;
        pos.position_open = false;
    }

private:
    struct Position {
        bool position_open = false;
        double entry_price = 0.0;
        double size = 1.0;
        bool is_long = true;
        double pnl = 0.0;
    };

public:
    const Position& position(int idx) const { return _positions[idx]; }

    double zscore_threshold() const { return _zscore_threshold; }

    size_t _num_symbols;
    double _zscore_threshold;
    std::vector<RollingWindow<double>> _windows;
    std::vector<ArbitrageSignal> _signals;
    std::vector<Position> _positions;
};


// ---------------------------------------------------------------------------
// Binary packet parsing (matches Python ABIDES build_packet layout)
// ---------------------------------------------------------------------------
struct SymbolData {
    std::string symbol;
    int32_t num_levels = 0;
    int64_t best_bid = 0;
    int64_t best_ask = 0;
    int64_t spread = 0;
};

bool parse_packet(const std::string& payload, std::vector<SymbolData>& out) {
    out.clear();
    if (payload.size() < 4) return false;
    uint32_t num_symbols = *reinterpret_cast<const uint32_t*>(payload.data());
    size_t offset = 4;
    for (uint32_t i = 0; i < num_symbols; ++i) {
        // symbol_name: char[16]
        if (offset + 16 > payload.size()) return false;
        SymbolData sd;
        char sym_buf[17] = {0};
        std::memcpy(sym_buf, payload.data() + offset, 16);
        sd.symbol = sym_buf;
        // Trim at null
        auto null_pos = sd.symbol.find('\0');
        if (null_pos != std::string::npos) sd.symbol = sd.symbol.substr(0, null_pos);
        offset += 16;

        // num_levels: uint32
        if (offset + 4 > payload.size()) return false;
        sd.num_levels = *reinterpret_cast<const uint32_t*>(payload.data() + offset);
        offset += 4;

        // best_bid: int64
        if (offset + 8 > payload.size()) return false;
        sd.best_bid = *reinterpret_cast<const int64_t*>(payload.data() + offset);
        offset += 8;

        // best_ask: int64
        if (offset + 8 > payload.size()) return false;
        sd.best_ask = *reinterpret_cast<const int64_t*>(payload.data() + offset);
        offset += 8;

        // spread: int64
        if (offset + 8 > payload.size()) return false;
        sd.spread = *reinterpret_cast<const int64_t*>(payload.data() + offset);
        offset += 8;

        // Skip levels: num_levels * (8 + 8)
        offset += static_cast<size_t>(sd.num_levels) * 16;

        out.push_back(std::move(sd));
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = Config{}.socket_path;
    double clock_hz = Config{}.clock_hz;
    int32_t threshold_scaled = Config{}.threshold_scaled;
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--clock-hz" && i + 1 < argc) {
            clock_hz = std::stod(argv[++i]);
        } else if (arg == "--threshold" && i + 1 < argc) {
            threshold_scaled = static_cast<int32_t>(std::stoll(argv[++i]));
        } else if (arg == "--symbols" && i + 1 < argc) {
            symbols.clear();
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                symbols.push_back(argv[++i]);
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: exchange_node [options]\n"
                      << "  --socket PATH     Unix domain socket path (default: "
                      << Config{}.socket_path << ")\n"
                      << "  --clock-hz HZ     RTL clock frequency in Hz (default: 250000000)\n"
                      << "  --threshold VAL   Spread threshold in scaled units (default: "
                      << Config{}.threshold_scaled << ")\n"
                      << "  --symbols SYM...  Symbols to track (default: BTCUSDT ETHUSDT SOLUSDT)\n";
            return 0;
        }
    }

#ifdef _WIN32
    signal(SIGINT, signal_handler);
#else
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
#endif

    TerminalPlot plot;
    plot.set_clock_hz(clock_hz);
    plot.set_threshold(threshold_scaled);
    plot.set_symbols(symbols);
    TerminalPlot::init_screen();

    double clock_period_ns = 1e9 / clock_hz;

    std::unique_ptr<Vspread_detector> rtl = std::make_unique<Vspread_detector>();

    rtl->clk = 0;
    rtl->rst_n = 0;
    rtl->s_axis_tvalid = 0;
    rtl->s_axis_tdata = 0;
    rtl->eval();

    rtl->clk = 1;
    rtl->eval();
    rtl->clk = 0;
    rtl->eval();
    rtl->rst_n = 1;
    rtl->eval();

    std::cout << "[MAIN] RTL reset complete. Clock = "
              << (clock_hz / 1e6) << " MHz (period "
              << clock_period_ns << " ns)\n";
    std::cout << "[MAIN] Threshold = " << threshold_scaled
              << " scaled units (" << (threshold_scaled / 1e8) << " USD)\n";
    std::cout << "[MAIN] Connecting to UDS: " << socket_path << "\n";
    std::cout << "[MAIN] Press Ctrl+C to stop.\n\n";

    SimpleArbitrageStrategy strategy(symbols.size());

    UdsSocketReader reader(socket_path);
    if (!reader.connected()) {
        std::cerr << "[MAIN] Failed to connect to UDS socket. "
                  << "Make sure the Python ABIDES simulator is running.\n";
        TerminalPlot::shutdown_screen();
        return 1;
    }

    std::vector<SymbolData> symbol_data;
    uint64_t update_count = 0;
    auto last_render = std::chrono::steady_clock::now();
    const auto render_interval = std::chrono::milliseconds(100);

    while (g_running.load()) {
        std::string payload;
        std::error_code ec;
        int n = reader.read_packet(payload, ec);

        if (n < 0) {
            if (ec == std::errc::timed_out ||
#ifdef _WIN32
                ec == std::error_code(WSAETIMEDOUT, std::system_category())
#else
                ec == std::errc::timed_out
#endif
            ) {
                continue;
            }
            std::cerr << "[MAIN] UDS read error: " << ec.message() << "\n";
            break;
        }

        if (!parse_packet(payload, symbol_data)) {
            std::cerr << "[MAIN] Failed to parse packet (" << n << " bytes)\n";
            continue;
        }

        for (const auto& sd : symbol_data) {
            // Find symbol index
            int sym_idx = -1;
            for (size_t i = 0; i < symbols.size(); ++i) {
                if (symbols[i] == sd.symbol) {
                    sym_idx = static_cast<int>(i);
                    break;
                }
            }

            // Compute midprice from best bid and ask (scaled units)
            double midprice = 0.0;
            if (sd.best_bid != 0 && sd.best_ask != 0) {
                midprice = (static_cast<double>(sd.best_bid) +
                            static_cast<double>(sd.best_ask)) / 2.0 / 1e8;
            }

            // Update strategy with midprice
            if (sym_idx >= 0 && midprice > 0) {
                strategy.update(midprice, sym_idx);
            }

            // Check for arbitrage signals
            const auto& sig = strategy.signal(sym_idx);

            if (sig.long_signal) {
                // Go long
                strategy.open_position(sig, midprice);
                // Drive RTL with z-score (scaled)
                rtl->s_axis_tdata = static_cast<int32_t>(sig.zscore * 1e8);
                rtl->s_axis_tvalid = 1;
                rtl->eval();
                rtl->clk = 1;
                rtl->eval();
                rtl->s_axis_tvalid = 0;
                rtl->clk = 0;
                rtl->eval();
                double latency_ns = 0;
                bool breached = (rtl->threshold_breached_o != 0);

                plot.feed(sd.symbol, sd.spread / 1e8, breached, 0, 0);
                plot.feed_arb_signal(sd.symbol, true, sig.zscore,
                                     midprice, midprice, false);

                std::cout << "[ARBITRAGE] " << sd.symbol << " Z=" << sig.zscore
                          << " -> LONG @ " << midprice << "\n";
            } else if (sig.short_signal) {
                // Go short
                strategy.open_position(sig, midprice);
                rtl->s_axis_tdata = static_cast<int32_t>(sig.zscore * 1e8);
                rtl->s_axis_tvalid = 1;
                rtl->eval();
                rtl->clk = 1;
                rtl->eval();
                rtl->s_axis_tvalid = 0;
                rtl->clk = 0;
                rtl->eval();
                double latency_ns = 0;
                bool breached = (rtl->threshold_breached_o != 0);

                plot.feed(sd.symbol, sd.spread / 1e8, breached, 0, 0);
                plot.feed_arb_signal(sd.symbol, true, sig.zscore,
                                     midprice, midprice, false);

                std::cout << "[ARBITRAGE] " << sd.symbol << " Z=" << sig.zscore
                          << " -> SHORT @ " << midprice << "\n";
            } else if (sig.close_signal) {
                // Close position
                strategy.close_position(sym_idx, midprice);
                plot.feed_arb_signal(sd.symbol, false, sig.zscore,
                                     sig.entry_price, midprice, true);
                std::cout << "[ARBITRAGE] " << sd.symbol << " Z=" << sig.zscore
                          << " -> CLOSE @ " << midprice
                          << " PnL: " << strategy.position(sym_idx).pnl << "\n";
            }

            // Update plot with latest spread data
            bool breached = false;
            if (sym_idx >= 0) {
                breached = (rtl->threshold_breached_o != 0);
            }
            plot.feed(sd.symbol, sd.spread / 1e8, breached, 0, 0);

            update_count++;
        }

        update_count++;

        auto now = std::chrono::steady_clock::now();
        if (now - last_render >= render_interval) {
            plot.render();
            last_render = now;
        }

        if (n == 0) {
#ifdef _WIN32
            Sleep(1);
#else
            usleep(100);
#endif
        }
    }

    std::cout << "\n[MAIN] Shutting down... processed " << update_count
              << " updates.\n";
    TerminalPlot::shutdown_screen();
    reader.close();

    return 0;
}