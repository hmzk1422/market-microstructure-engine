#include "config.h"
#include "socket_reader.h"
#include "terminal_plot.h"
#include "verilator_hooked.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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
// Rolling window for z-score computation (pipeline level, not strategy)
// ---------------------------------------------------------------------------
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
    double zscore(double current) const {
        double m = mean();
        double s = stddev();
        if (s == 0.0) return 0.0;
        return (static_cast<double>(current) - m) / s;
    }

private:
    size_t _capacity;
    std::deque<double> _data;
};

// ---------------------------------------------------------------------------
// Symbol data parsed from UDS packet
// ---------------------------------------------------------------------------
struct SymbolData {
    std::string symbol;
    int32_t spread;       // scaled units (1e8)
    int32_t best_bid;     // scaled units (1e8)
    int32_t best_ask;     // scaled units (1e8)
    int sym_idx = -1;
};

// Parse a UDS packet line into SymbolData
// Expected format: "SYMBOL spread best_bid best_ask" (all scaled ints)
SymbolData parse_packet(const std::string& line, const std::vector<std::string>& symbols) {
    SymbolData sd;
    sd.sym_idx = -1;

    size_t pos1 = line.find(' ');
    if (pos1 == std::string::npos) return sd;
    sd.symbol = line.substr(0, pos1);

    size_t pos2 = line.find(' ', pos1 + 1);
    if (pos2 == std::string::npos) return sd;
    sd.spread = std::stoi(line.substr(pos1 + 1, pos2 - pos1 - 1));

    size_t pos3 = line.find(' ', pos2 + 1);
    if (pos3 == std::string::npos) return sd;
    sd.best_bid = std::stoi(line.substr(pos2 + 1, pos3 - pos2 - 1));

    sd.best_ask = std::stoi(line.substr(pos3 + 1));

    for (size_t i = 0; i < symbols.size(); ++i) {
        if (symbols[i] == sd.symbol) {
            sd.sym_idx = static_cast<int>(i);
            break;
        }
    }

    return sd;
}

} // namespace

int main(int argc, char** argv) {
    // Parse command line arguments
    std::string socket_path = "/tmp/market_simulator.sock";
    double clock_hz = 250'000'000.0;
    int32_t threshold_scaled = 5000;
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--clock-hz" && i + 1 < argc) {
            clock_hz = std::stod(argv[++i]);
        } else if (arg == "--threshold" && i + 1 < argc) {
            threshold_scaled = std::stoi(argv[++i]);
        } else if (arg == "--symbols") {
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                symbols.push_back(argv[++i]);
            }
        }
    }

    // Install signal handler for graceful shutdown
    signal(SIGINT, signal_handler);

    // Initialize RTL (Verilated spread detector)
    auto rtl = std::make_unique<Vspread_detector>();

    // Reset RTL
    rtl->rst_n = 0;
    rtl->clk = 0;
    rtl->s_axis_tdata = 0;
    rtl->s_axis_tvalid = 0;
    rtl->eval();

    rtl->clk = 1; rtl->eval();
    rtl->clk = 0; rtl->eval();
    rtl->rst_n = 1; rtl->eval();

    // Initialize terminal plot
    TerminalPlot plot(80, 50);
    plot.set_threshold(threshold_scaled);
    plot.set_clock_hz(clock_hz);
    plot.set_symbols(symbols);
    plot.init_screen();
    plot.hide_cursor();

    // Per-symbol rolling windows for z-score computation (pipeline level)
    std::vector<RollingWindow> windows;
    windows.reserve(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) {
        windows.emplace_back(50); // 50-sample rolling window
    }

    // Initialize UDS socket reader (constructor auto-connects)
    UdsSocketReader reader(socket_path);
    if (!reader.connected()) {
        std::cerr << "[FATAL] Failed to connect to exchange: " << socket_path << "\n";
        return 1;
    }

    std::cout << "[MAIN] RTL reset complete. Clock = " << (clock_hz / 1e6) << " MHz\n";
    std::cout << "[MAIN] Threshold = " << threshold_scaled << " scaled units\n";
    std::cout << "[MAIN] Connected to UDS: " << socket_path << "\n";
    std::cout << "[MAIN] Press Ctrl+C to stop.\n\n";

    // Main ingestion loop — pipeline only: read → parse → RTL → plot
    // No arbitrage strategy here. Strategy lives on the user side (FPGA side),
    // connected to Verilator RTL separately.
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

        SymbolData sd = parse_packet(payload, symbols);
        if (sd.sym_idx < 0) continue;

        // Compute z-score from spread (pipeline level)
        double spread_scaled = static_cast<double>(sd.spread) / 1e8;
        double z = windows[sd.sym_idx].zscore(spread_scaled);
        windows[sd.sym_idx].push_back(spread_scaled);

        // Feed z-score * 1e8 to RTL (fixed-point for hardware)
        rtl->s_axis_tdata = static_cast<int32_t>(z * 1e8);
        rtl->s_axis_tvalid = 1;
        rtl->eval();
        rtl->clk = 1;
        rtl->eval();
        rtl->s_axis_tvalid = 0;
        rtl->clk = 0;
        rtl->eval();

        // Read threshold breach from RTL (FPGA side)
        bool breached = (rtl->threshold_breached_o != 0);

        // Feed to terminal plot (ingestion + RTL + display only)
        plot.feed(sd.symbol, spread_scaled, breached, 0, 0);

        update_count++;

        auto now = std::chrono::steady_clock::now();
        if (now - last_render >= render_interval) {
            plot.render();
            last_render = now;
        }

#ifdef _WIN32
        Sleep(1);
#else
        usleep(100);
#endif
    }

    std::cout << "\n[MAIN] Shutting down... processed " << update_count << " updates.\n";
    TerminalPlot::shutdown_screen();
    reader.close();

    return 0;
}