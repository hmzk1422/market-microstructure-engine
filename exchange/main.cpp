#include "config.h"
#include "socket_reader.h"
#include "terminal_plot.h"
#include "verilator_hooked.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
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

struct SymbolData {
    std::string symbol;
    int64_t best_bid = 0;
    int64_t best_ask = 0;
    int64_t spread = 0;
    bool has_data = false;
};

bool parse_packet(const std::string& payload,
                  std::vector<SymbolData>& out_symbols) {
    const char* ptr = payload.data();
    const char* end = payload.data() + payload.size();

    if (static_cast<size_t>(end - ptr) < 4) return false;
    uint32_t num_symbols = *reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4;

    out_symbols.clear();
    out_symbols.reserve(num_symbols);

    for (uint32_t i = 0; i < num_symbols; ++i) {
        SymbolData sd;

        if (static_cast<size_t>(end - ptr) < 16) return false;
        sd.symbol.assign(ptr, 16);
        auto null_pos = sd.symbol.find('\0');
        if (null_pos != std::string::npos) {
            sd.symbol.resize(null_pos);
        }
        ptr += 16;

        if (static_cast<size_t>(end - ptr) < 4) return false;
        uint32_t num_levels = *reinterpret_cast<const uint32_t*>(ptr);
        ptr += 4;
        (void)num_levels;

        if (static_cast<size_t>(end - ptr) < 24) return false;
        sd.best_bid = *reinterpret_cast<const int64_t*>(ptr);
        ptr += 8;
        sd.best_ask = *reinterpret_cast<const int64_t*>(ptr);
        ptr += 8;
        sd.spread = *reinterpret_cast<const int64_t*>(ptr);
        ptr += 8;

        ptr += static_cast<size_t>(num_levels) * 16;

        sd.has_data = true;
        out_symbols.push_back(std::move(sd));
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
            uint64_t cycles = 0;

            rtl->clk = 0;
            rtl->s_axis_tdata = static_cast<int32_t>(sd.spread);
            rtl->s_axis_tvalid = 1;
            rtl->eval();
            ++cycles;

            rtl->clk = 1;
            rtl->eval();
            ++cycles;

            rtl->s_axis_tvalid = 0;
            rtl->clk = 0;
            rtl->eval();

            bool breached = (rtl->threshold_breached_o != 0);
            double latency_ns = cycles * clock_period_ns;

            plot.feed(sd.symbol, static_cast<double>(sd.spread), breached,
                      cycles, latency_ns);

            (void)breached;
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