#include "terminal_plot.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>

TerminalPlot::TerminalPlot(int width, int history)
    : _width(width), _history(history), _threshold_scaled(5000),
      _clock_hz(250'000'000.0) {}

void TerminalPlot::set_threshold(int32_t threshold_scaled) {
    _threshold_scaled = threshold_scaled;
}

void TerminalPlot::set_clock_hz(double clock_hz) {
    _clock_hz = clock_hz;
}

void TerminalPlot::set_symbols(const std::vector<std::string>& symbols) {
    _symbols = symbols;
    _states.clear();
    _states.reserve(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) {
        SymbolState st;
        st.history.resize(static_cast<size_t>(_history), PlotPoint{0.0, false});
        _states.push_back(std::move(st));
    }
}

void TerminalPlot::init_screen() {
    std::cout << "\033[2J\033[?25l" << std::flush;
}

void TerminalPlot::shutdown_screen() {
    std::cout << "\033[?25h\033[2J\033[H" << std::flush;
}


void TerminalPlot::feed(const std::string& symbol, double spread_scaled,
                        bool breached, uint64_t cycles, double latency_ns) {

    size_t idx = _states.size();

    for (size_t i = 0; i < _symbols.size(); ++i) {

        if (_symbols[i] == symbol) {

            idx = i;

            break;

        }

    }

    if (idx >= _states.size()) {

        return;

    }

    auto& st = _states[idx];
    st.history.push_back(PlotPoint{spread_scaled, breached});
    if (static_cast<int>(st.history.size()) > _history) {

        st.history.pop_front();
    }
    st.last_spread = spread_scaled;
    st.last_breached = breached;
    st.last_cycles = cycles;
    st.last_latency_ns = latency_ns;
}

void TerminalPlot::feed_arb_signal(const std::string& symbol, bool open,
                                   double zscore_scaled, double entry_price,
                                   double exit_price, bool closed) {

    size_t idx = _states.size();

    for (size_t i = 0; i < _symbols.size(); ++i) {

        if (_symbols[i] == symbol) {

            idx = i;

            break;

        }

    }

    if (idx >= _states.size()) {

        return;

    }

    auto& st = _states[idx];
    st.last_zscore = zscore_scaled;
    st.last_arb_open = open;

    if (closed) {
        double trade_pnl = (exit_price - entry_price) * st.position.size;
        if (!st.position.is_long) {
            trade_pnl = -trade_pnl;
        }
        st.total_pnl += trade_pnl;
        st.position = Position{};
    } else if (open) {
        st.position.entry_price = entry_price;
        st.position.size = 1.0;
        st.position.is_long = true;
        st.position.open = true;
    } else {
        st.position.open = false;
    }
}
void TerminalPlot::clear_screen() {
    std::cout << "\033[2J\033[H" << std::flush;
}

void TerminalPlot::home_cursor() {
    std::cout << "\033[H" << std::flush;
}

void TerminalPlot::set_color(int code) {
    std::cout << "\033[" << code << "m" << std::flush;
}

void TerminalPlot::hide_cursor() {
    std::cout << "\033[?25l" << std::flush;
}

void TerminalPlot::show_cursor() {
    std::cout << "\033[?25h" << std::flush;
}

void TerminalPlot::render() const {
    home_cursor();
    double clock_period_ns = 1e9 / _clock_hz;
    set_color(1);
    std::cout << "============================================================================\n";
    std::cout << "  MARKET MICROSTRUCTURE ENGINE  |  Real-time Arbitrage Monitor\n";
    std::cout << "  Clock: " << (_clock_hz / 1e6) << " MHz  |  Period: "
              << clock_period_ns << " ns  |  Z-Score Threshold: "
              << (_threshold_scaled / 1e8) << " (scaled units)\n";
    std::cout << "============================================================================\n";
    set_color(0);
    std::cout << "  Total Accumulated PnL: ";
    set_color(32);
    std::cout << " $" << std::fixed << std::setprecision(2) << _states[0].total_pnl
              << "  ";
    set_color(0);
    std::cout << "\n\n";
    std::cout << "  Symbol   | Spread (scaled) | Spread (USD)  | Best Bid   | Best Ask   \n";
    std::cout << "  ---------|-----------------|---------------|------------|------------\n";
    std::cout << "  --------|-----------|------------|------------|-----------\n";
    double max_spread = 0.0;
    for (const auto& st : _states) {
        max_spread = std::max(max_spread, st.last_spread);
    }
    max_spread = std::max(max_spread, static_cast<double>(_threshold_scaled));
    if (max_spread == 0.0) max_spread = 1.0;
    for (size_t i = 0; i < _states.size(); ++i) {
        const auto& st = _states[i];
        const auto& sym = _symbols[i];
        std::cout << "  " << sym << "  | ";
        std::cout.precision(4);
        std::cout << st.last_spread;
        for (int pad = 16 - static_cast<int>(std::to_string(
                         static_cast<long long>(st.last_spread * 10000)).size()); pad > 0; --pad) {
            std::cout << ' ';
        }
        std::cout << " | ";
        std::cout << st.last_spread / 1e8;
        for (int pad = 13 - static_cast<int>(std::to_string(
                         static_cast<long long>(st.last_spread / 1e8 * 10000)).size()); pad > 0; --pad) {
            std::cout << ' ';
        }
        std::cout << " | ";
        std::cout << st.best_bid / 1e8;
        for (int pad = 10 - static_cast<int>(std::to_string(
                         static_cast<long long>(st.best_bid / 1e8 * 10000)).size()); pad > 0; --pad) {
            std::cout << ' ';
        }
        std::cout << " | ";
        std::cout << st.best_ask / 1e8;
        for (int pad = 10 - static_cast<int>(std::to_string(
                         static_cast<long long>(st.best_ask / 1e8 * 10000)).size()); pad > 0; --pad) {
            std::cout << ' ';
        }
        std::cout << " | ";
        std::cout.precision(2);
        std::cout << st.last_zscore;
        for (int pad = 8 - static_cast<int>(std::to_string(
                         static_cast<long long>(st.last_zscore * 100)).size()); pad > 0; --pad) {
            std::cout << ' ';
        }
        std::cout << " | ";
        std::cout.precision(0);
        std::cout << st.last_cycles;
        for (int pad = 8 - static_cast<int>(std::to_string(
                         static_cast<long long>(st.last_cycles)).size()); pad > 0; --pad) {
            std::cout << ' ';
        }
        std::cout << " | ";
        std::cout.precision(1);
        std::cout << st.last_latency_ns;
        for (int pad = 10 - static_cast<int>(std::to_string(
                         static_cast<long long>(st.last_latency_ns * 10)).size()); pad > 0; --pad) {
            std::cout << ' ';
        }
        std::cout << " | ";
        if (st.position.open) {
            std::cout << (st.position.is_long ? "LONG   " : "SHORT  ");
        } else if (st.last_arb_open) {
            std::cout << "OPEN     ";
        } else {
            std::cout << "WAITING  ";
        }
        std::cout << " | ";
        std::cout.precision(2);
        std::cout << st.total_pnl;
        std::cout << "\n";
    }
    std::cout << "\n  Sparkline (recent spreads, last " << _history << " updates):\n";
    for (size_t i = 0; i < _states.size(); ++i) {
        const auto& st = _states[i];
        const auto& sym = _symbols[i];
        std::cout << "  " << sym << " Z-Score: " << sparkline(st.history) << "\n";
    }
    std::cout << "\n  Legend:  ";
    set_color(32); std::cout << "[OK]"; set_color(0);
    std::cout << "   ";
    set_color(31); std::cout << "[Z-SCORE BREACHED (|z| >= 2)]"; set_color(0);
    std::cout << "   |   Position: Long (mean-reversion)\n";
    std::cout << "  PnL = cumulative (exit_price - entry_price) * position_size\n";
    std::cout << "  Z-score = (current_price - mean) / std (over rolling window)\n";
    std::cout << "----------------------------------------------------------------------------\n";
    std::cout.flush();
}

std::string TerminalPlot::sparkline(const std::deque<PlotPoint>& hist) const {
    const char* levels = " ▁▂▃▄▅▆▇█";
    int nlevels = 8;
    double max_val = static_cast<double>(_threshold_scaled);
    for (const auto& p : hist) {
        max_val = std::max(max_val, p.value);
    }
    if (max_val == 0.0) max_val = 1.0;
    std::string out;
    out.reserve(hist.size());
    for (const auto& p : hist) {
        int level = static_cast<int>((p.value / max_val) * (nlevels - 1) + 0.5);
        if (level < 0) level = 0;
        if (level >= nlevels) level = nlevels - 1;
        if (p.value >= static_cast<double>(_threshold_scaled)) {
            set_color(31);
        } else {
            set_color(32);
        }
        out += levels[level];
    }
    set_color(0);
    return out;
}

std::string TerminalPlot::bar_chart(double /*value*/, double /*max_value*/,
                                    int /*bar_w*/) const {
    return "";
}