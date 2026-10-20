#pragma once

#include <cstdint>

// Configuration for the market-microstructure-engine exchange node.
//
// This file centralizes all tunable parameters: clock frequency, price scale,
// threshold, UDS path, and simulation parameters.

struct Config {
    // -- Clock / timing ---------------------------------------------------
    // Target clock frequency in Hz. Default 250 MHz -> clock period 4 ns.
    double clock_hz = 250'000'000.0;
    double clock_period_ns = 4.0;   // computed: 1e9 / clock_hz

    // -- Price / spread scaling -------------------------------------------
    // Price and size are scaled by this factor (1e8 = 1.00000000).
    double price_scale = 1e8;

    // -- Arbitrage threshold ----------------------------------------------
    // Spread (in scaled units) that triggers the hardware alert.
    // The RTL stores THRESHOLD as a fixed parameter; this is the C++ value
    // used for simulation, testing, and display purposes.
    int32_t threshold_scaled = 5000;   // e.g. 50.00 in 1e8 units

    // -- Unix domain socket -----------------------------------------------
    const char* socket_path = "/tmp/market_simulator.sock";

    // -- UDS readback timeout (seconds); after this, stop gracefully.
    double read_timeout_sec = 30.0;

    // -- Data fields ------------------------------------------------------
    int orderbook_depth = 20;
};
