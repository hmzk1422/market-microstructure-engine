#include <iostream>
#include <memory>
#include <deque>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <system_error>

#ifdef _WIN32
#define _WINSOCKAPI_
#include <signal.h>
#include <windows.h>
#undef _WINSOCKAPI_
#else
#include <csignal>
#include <unistd.h>
#endif

#include "../../exchange/verilator_hooked.h"

#include "../../exchange/socket_reader.h"
#include "../../exchange/config.h"
#include "../../exchange/terminal_plot.h"

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int /*sig*/) {
    g_running.store(false);
}

class RollingWindow {
public:
    explicit RollingWindow(size_t capacity) : _capacity(capacity) {}

    void push_back(double value) {
        _data.push_back(value);
        if (_data.size() > _capacity) {
            _data.pop_front();
        }
    }

    size_t size() const { return _data.size(); }

    double mean() const {
        if (_data.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& v : _data) sum += v;
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

    double zscore(double current) const {
        double m = mean();
        double s = stddev();
        if (s == 0.0) return 0.0;
        return (current - m) / s;
    }

private:
    size_t _capacity;
    std::deque<double> _data;
};

struct SymbolData {
    std::string symbol;
    int64_t spread;
    int64_t best_bid;
    int64_t best_ask;
    int sym_idx = -1;
};

std::vector<SymbolData> parse_packet(const std::string& payload,
                                     const std::vector<std::string>& symbols) {
    std::vector<SymbolData> result;
    const char* data = payload.data();
    size_t len = payload.size();
    size_t offset = 0;

    while (offset < len) {
        // Need at least: name[16] + num_levels[4] + best_bid[8] + best_ask[8] + spread[8] = 44 bytes
        if (offset + 44 > len) break;

        SymbolData sd;
        sd.sym_idx = -1;

        // symbol name: char[16]
        char name_buf[17] = {0};
        std::memcpy(name_buf, data + offset, 16);
        sd.symbol = std::string(name_buf);
        size_t null_pos = sd.symbol.find('\0');
        if (null_pos != std::string::npos) sd.symbol.resize(null_pos);
        offset += 16;

        // num_levels: uint32
        uint32_t num_levels = 0;
        std::memcpy(&num_levels, data + offset, 4);
        offset += 4;

        // best_bid: int64
        std::memcpy(&sd.best_bid, data + offset, 8);
        offset += 8;

        // best_ask: int64
        std::memcpy(&sd.best_ask, data + offset, 8);
        offset += 8;

        // spread: int64
        std::memcpy(&sd.spread, data + offset, 8);
        offset += 8;

        // skip levels: (price int64, size int64) x num_levels
        offset += static_cast<size_t>(num_levels) * 16;

        // find sym_idx
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (symbols[i] == sd.symbol) {
                sd.sym_idx = static_cast<int>(i);
                break;
            }
        }

        result.push_back(sd);
    }

    return result;
}

} // namespace

int main(int argc, char** argv) {

    std::string host = "127.0.0.1";
    int port = 6789;
    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::stoi(argv[2]);

    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT"};

    signal(SIGINT, signal_handler);

    auto rtl = std::make_unique<Vspread_detector>();
    rtl->rst_n = 0;
    rtl->clk = 0;
    rtl->s_axis_tdata = 0;
    rtl->s_axis_tvalid = 0;
    rtl->eval();

    rtl->clk = 1; rtl->eval();
    rtl->clk = 0; rtl->eval();
    rtl->rst_n = 1; rtl->eval();

    UdsSocketReader reader(Config{}.socket_path, port);
    if (!reader.connected()) {
#ifdef _WIN32
        std::cerr << "[FATAL] Failed to connect to " << host << ":" << port << "\n";
#else
        std::cerr << "[FATAL] Failed to connect to Unix socket: "
                  << Config{}.socket_path << "\n";
#endif
        return 1;
    }

#ifdef _WIN32
    std::cout << "[STRATEGY] Connected to exchange at " << host << ":" << port << "\n";
#else
    std::cout << "[STRATEGY] Connected to exchange socket "
              << Config{}.socket_path << "\n";
#endif
    std::cout << "[STRATEGY] Z-score threshold: " << Config{}.zscore_threshold << " sigma\n";
    std::cout << "[STRATEGY] Position size: " << Config{}.position_size << "\n";
    std::cout << "[STRATEGY] Press Ctrl+C to stop.\n\n";

    struct ArbState {
        RollingWindow window{50};
        Position position;
        double total_pnl = 0.0;
        double last_zscore = 0.0;
    };
    std::vector<ArbState> states(symbols.size());

    TerminalPlot plot(80, 50);
    plot.set_threshold(Config{}.threshold_scaled);
    plot.set_clock_hz(Config{}.clock_hz);
    plot.set_symbols(symbols);
    plot.init_screen();
    TerminalPlot::hide_cursor();

    int update_count = 0;
    auto last_render = std::chrono::steady_clock::now();
    const auto render_interval = std::chrono::milliseconds(200);

    while (g_running.load()) {
        std::string payload;
        std::error_code ec;
        int n = reader.read_packet(payload, ec);
        if (n < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto parsed = parse_packet(payload, symbols);
        for (const auto& sd : parsed) {
            if (sd.sym_idx < 0) continue;

            double spread_scaled = static_cast<double>(sd.spread) / 1e8;
            ArbState& st = states[sd.sym_idx];
            double z = st.window.zscore(spread_scaled);
            st.window.push_back(spread_scaled);
            st.last_zscore = z;

            rtl->s_axis_tdata = static_cast<int32_t>(z * 1e8);
            rtl->s_axis_tvalid = 1;
            rtl->eval();
            rtl->clk = 1;
            rtl->eval();
            rtl->s_axis_tvalid = 0;
            rtl->clk = 0;
            rtl->eval();

            bool breached = (rtl->threshold_breached_o != 0);

            double threshold = Config{}.zscore_threshold;

            if (!st.position.open) {
                if (breached) {
                    if (z < -threshold) {
                        st.position.is_long = true;
                        st.position.entry_price = spread_scaled;
                        st.position.size = Config{}.position_size;
                        st.position.open = true;
                        std::cout << "[ARB] " << sd.symbol
                                  << " LONG  @ z=" << z
                                  << " spread=" << spread_scaled << "\n";
                        plot.feed_arb_signal(sd.symbol, true, z, spread_scaled);
                    } else if (z > threshold) {
                        st.position.is_long = false;
                        st.position.entry_price = spread_scaled;
                        st.position.size = Config{}.position_size;
                        st.position.open = true;
                        std::cout << "[ARB] " << sd.symbol
                                  << " SHORT @ z=" << z
                                  << " spread=" << spread_scaled << "\n";
                        plot.feed_arb_signal(sd.symbol, true, z, spread_scaled);
                    }
                }
            } else {
                bool should_close = false;
                if (st.position.is_long) {
                    if (z >= 0.0 || !breached) should_close = true;
                } else {
                    if (z <= 0.0 || !breached) should_close = true;
                }

                if (should_close) {
                    double exit_price = spread_scaled;
                    double pnl = 0.0;
                    if (st.position.is_long) {
                        pnl = (exit_price - st.position.entry_price) * st.position.size;
                    } else {
                        pnl = (st.position.entry_price - exit_price) * st.position.size;
                    }
                    st.total_pnl += pnl;
                    std::cout << "[ARB] " << sd.symbol
                              << " CLOSE @ z=" << z
                              << " PnL=" << pnl
                              << " Total=" << st.total_pnl << "\n";
                    plot.feed_arb_signal(sd.symbol, false, z,
                                         st.position.entry_price, exit_price, true);
                    st.position = Position{};
                }
            }

            plot.feed(sd.symbol, spread_scaled, breached, 0, 0);
            update_count++;

            auto now = std::chrono::steady_clock::now();
            if (now - last_render >= render_interval) {
                plot.render();
                last_render = now;
            }
        } // end for each symbol in packet

#ifdef _WIN32
        Sleep(1);
#else
        usleep(100);
#endif
    }

    std::cout << "\n[STRATEGY] Shutting down... processed " << update_count
              << " updates.\n";
    TerminalPlot::shutdown_screen();
    reader.close();

    for (size_t i = 0; i < symbols.size(); ++i) {
        std::cout << "[STRATEGY] " << symbols[i]
                  << " Total PnL: " << states[i].total_pnl << "\n";
    }

    return 0;
}