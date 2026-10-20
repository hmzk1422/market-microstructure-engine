#include "todo_lob_simulator.h"

// TODO: Implement the pure-C++ LOB simulator.
//
// TODO(1) Build the OrderBook struct using std::map<double, double> for bids
//        (descending price) and asks (ascending price).
//
// TODO(2) Implement order insert / cancel / trade matching:
//        - insert_order(symbol, side, price, size)
//        - cancel_order(symbol, price_level)
//        - match_orders() -> returns executed trades
//
// TODO(3) Implement snapshot() and delta() serialization:
//        - serialize to the same binary format the Unix domain socket uses
//        - so the C++ app and the LOB simulator stay in sync
//
// TODO(4) Implement configuration:
//        - orderbook_depth (number of levels)
//        - price_scale (e.g. 1e8 to preserve precision)
//        - clock_period_ns (default 4.0 ns for 250 MHz)
//
// TODO(5) Add a main() or test harness that:
//        - builds a small orderbook
//        - inserts matched and unmatched orders
//        - prints spread and occupied levels per cycle at 250 MHz
//
// Design note: this file should be dependency-free (only standard library)
// so it can be dropped into the C++ application as a standalone LOB engine.
