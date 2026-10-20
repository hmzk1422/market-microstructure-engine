#pragma once

// TODO: LOB (Limit Order Book) simulator for the market-microstructure-engine.
//
// This component is intended to replace / complement the Python ABIDES exchange
// with a pure C++ orderbook implementation that can be driven in-process by the
// C++ application. It will own:
//
//   - L2 book state (bids + asks in price/size/uncertainty)
//   - Order insert / cancel / trade matching (simple limit order matching)
//   - Snapshot + delta generation for the Unix domain socket
//   - Configurable depth, price scaling, and clock period
//
// Current state: TODO (placeholder). Implement:
//   1. OrderBook struct (bids/asks as std::map<double, double>)
//   2. insert_order(), cancel_order(), match_orders()
//   3. snapshot() / delta() serialization for the socket
//   4. Config loading (depth, scale, clock period)
//   5. Unit tests for matching + spread calc
//
// Target: drives the Verilator spread_detector.sv at ~250 MHz.