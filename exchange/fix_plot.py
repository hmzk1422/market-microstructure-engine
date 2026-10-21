import io
with open('terminal_plot.cpp', 'rb') as f:
    data = f.read()

start = data.find(b'void TerminalPlot::render() const')
end = data.find(b'}', start)
end = data.find(b'}', end + 1)

print('Render method:', start, end)

before = data[:start]
after = data[end:]

new_render = b'''void TerminalPlot::render() const {
    home_cursor();
    double clock_period_ns = 1e9 / _clock_hz;
    set_color(1);
    std::cout << "============================================================================\\n";
    std::cout << "  MARKET MICROSTRUCTURE ENGINE  |  Real-time Arbitrage Monitor\\n";
    std::cout << "  Clock: " << (_clock_hz / 1e6) << " MHz  |  Period: "
              << clock_period_ns << " ns  |  Z-Score Threshold: "
              << (_threshold_scaled / 1e8) << " (scaled units)\\n";
    std::cout << "============================================================================\\n";
    set_color(0);
    std::cout << "  Total Accumulated PnL: ";
    set_color(32);
    std::cout << " $" << std::fixed << std::setprecision(2) << _states[0].total_pnl
              << "  ";
    set_color(0);
    std::cout << "\\n\\n";
    std::cout << "  Symbol   | Spread (scaled) | Spread (USD)  | Best Bid   | Best Ask   " << std::endl;
    std::cout << "  ---------|-----------------|---------------|------------|------------" << std::endl;
    std::cout << "  --------|-----------|------------|------------|-----------" << std::endl;
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
        std::cout << "\\n";
    }
    std::cout << "\\n  Sparkline (recent spreads, last " << _history << " updates):\\n";
    for (size_t i = 0; i < _states.size(); ++i) {
        const auto& st = _states[i];
        const auto& sym = _symbols[i];
        std::cout << "  " << sym << " Z-Score: " << sparkline_for_zscore(st.last_zscore) << "\\n";
    }
    std::cout << "\\n  Legend:  ";
    set_color(32); std::cout << "[OK]"; set_color(0);
    std::cout << "   ";
    set_color(31); std::cout << "[Z-SCORE BREACHED (|z| >= 2)]"; set_color(0);
    std::cout << "   |   Position: Long (mean-reversion)\\n";
    std::cout << "  PnL = cumulative (exit_price - entry_price) * position_size\\n";
    std::cout << "  Z-score = (current_price - mean) / std (over rolling window)\\n";
    std::cout << "----------------------------------------------------------------------------\\n";
    std::cout.flush();
}

'''

new_data = before + new_render + after
new_data = new_data.replace(b'\n', b'\r\n')
with open('terminal_plot.cpp', 'wb') as f:
    f.write(new_data)
print('Done, file size:', len(new_data))

