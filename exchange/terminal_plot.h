#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

/// A single data point for the real-time plot.
struct PlotPoint {
    double value;   // spread (scaled units)
    bool  breached; // threshold breached?
};

/// Position state for the arbitrage strategy.
struct Position {
    double entry_price = 0.0;  // scaled units (1e8)
    double size = 0.0;         // position size
    bool  is_long = true;      // true = long, false = short
    bool  open = false;        // is a position open?
};

/// Real-time terminal plot using ANSI escape codes.
/// Maintains a rolling window of spread values per symbol and renders
/// an ASCII sparkline + status line on each refresh.
class TerminalPlot {
public:
    explicit TerminalPlot(int width = 80, int history = 60);

    // Add a new data point for a symbol.
    void feed(const std::string& symbol, double spread_scaled,
              bool breached, uint64_t cycles, double latency_ns);

    // Add a new arbitrage signal for a symbol.
    void feed_arb_signal(const std::string& symbol, bool open,
                         double zscore_scaled, double entry_price = 0.0,
                         double exit_price = 0.0, bool closed = false);

    // Render the full plot to stdout (call each frame).
    void render() const;

    // Set configuration for display.
    void set_threshold(int32_t threshold_scaled);
    void set_clock_hz(double clock_hz);
    void set_symbols(const std::vector<std::string>& symbols);

    // Clear screen + hide cursor (call once at startup).
    static void init_screen();
    // Restore cursor (call once at shutdown).
    static void shutdown_screen();

    static void hide_cursor();
    static void show_cursor();

private:
    int _width;
    int _history;
    int32_t _threshold_scaled;
    double _clock_hz;

    struct SymbolState {
        std::deque<PlotPoint> history;
        double last_spread = 0.0;
        double best_bid = 0.0;
        double best_ask = 0.0;
        uint64_t last_cycles = 0;
        double last_latency_ns = 0.0;
        bool last_breached = false;

        // Arbitrage state
        Position position;
        double last_zscore = 0.0;
        double total_pnl = 0.0;
        bool last_arb_open = false;
    };

    std::vector<std::string> _symbols;
    std::vector<SymbolState> _states;

    // ANSI helpers
    static void clear_screen();
    static void home_cursor();
    static void set_color(int code);

    // Render helpers
    std::string sparkline(const std::deque<PlotPoint>& hist) const;
    std::string bar_chart(double value, double max_value, int bar_w) const;
};