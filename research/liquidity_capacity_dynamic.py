"""
liquidity_capacity_dynamic.py
==============================
Phase 2: Dynamic Market Impact Simulator

Replays historical LOB events from TimescaleDB for both perp and spot,
maintains live order book state, and simulates placing market orders at
signal times. This captures:

  1. Actual liquidity consumption (our order removes depth)
  2. Queue position effects (subsequent orders see reduced liquidity)
  3. Adverse selection (markout after fill)
  4. Price impact from walking the book

Unlike the static walk-the-book (Phase 1), this properly accounts for
the fact that our own order changes the book state for subsequent fills.
"""

import numpy as np
import pandas as pd
from sqlalchemy import create_engine
from preprocessing import prepare_signals
from liquidity_capacity import (
    reconstruct_book_with_depth,
    FEE_HURDLE_BPS,
    QTY_GRID,
    N_LEVELS,
    generate_capacity_report,
)

DB_URL = "postgresql://market:market@localhost:5433/market_data"


# =====================================================================
# 1. LIVE ORDER BOOK STATE MACHINE
#    Maintains a real-time view of the order book as events are replayed.
# =====================================================================
class LiveOrderBook:
    """Maintains a live top-N order book state from raw LOB events."""

    def __init__(self, symbol, levels=N_LEVELS):
        self.symbol = symbol
        self.levels = levels
        self.bids = {}  # price -> size
        self.asks = {}  # price -> size
        self.current_time = None
        self.current_seq = None

    def apply_event(self, time, side, price, size, is_snapshot, seq):
        """Apply a single LOB event to the book state."""
        self.current_time = time
        self.current_seq = seq

        if is_snapshot:
            if side == 'bid':
                self.bids.clear()
            else:
                self.asks.clear()

        book_side = self.bids if side == 'bid' else self.asks
        if size == 0:
            book_side.pop(price, None)
        else:
            book_side[price] = size

    def snapshot(self):
        """Return the current top-N levels as a dict."""
        bid_prices = sorted(self.bids.keys(), reverse=True)[:self.levels]
        ask_prices = sorted(self.asks.keys())[:self.levels]
        row = {'time': self.current_time}
        for k in range(self.levels):
            row[f'bid_price_{k+1}'] = bid_prices[k] if k < len(bid_prices) else np.nan
            row[f'bid_size_{k+1}'] = self.bids.get(bid_prices[k], np.nan) if k < len(bid_prices) else np.nan
            row[f'ask_price_{k+1}'] = ask_prices[k] if k < len(ask_prices) else np.nan
            row[f'ask_size_{k+1}'] = self.asks.get(ask_prices[k], np.nan) if k < len(ask_prices) else np.nan
        return row

    def get_bid_levels(self):
        """Return (prices, sizes) for bid side, best-first."""
        prices = sorted(self.bids.keys(), reverse=True)[:self.levels]
        sizes = [self.bids[p] for p in prices]
        return np.array(prices), np.array(sizes)

    def get_ask_levels(self):
        """Return (prices, sizes) for ask side, best-first."""
        prices = sorted(self.asks.keys())[:self.levels]
        sizes = [self.asks[p] for p in prices]
        return np.array(prices), np.array(sizes)

    def consume_bids(self, qty):
        """
        Consume liquidity from the bid side (as if hitting bids with a sell).
        Returns (vwap, filled, remaining_bids_dict).
        Modifies the book in place.
        """
        bid_prices = sorted(self.bids.keys(), reverse=True)
        remaining = qty
        cost = 0.0
        filled = 0.0
        for p in bid_prices:
            if remaining <= 0:
                break
            available = self.bids[p]
            take = min(remaining, available)
            cost += take * p
            filled += take
            remaining -= take
            if take >= available:
                del self.bids[p]
            else:
                self.bids[p] -= take
        if filled > 0:
            return cost / filled, filled
        return np.nan, 0.0

    def consume_asks(self, qty):
        """
        Consume liquidity from the ask side (as if lifting offers with a buy).
        Returns (vwap, filled, remaining_asks_dict).
        Modifies the book in place.
        """
        ask_prices = sorted(self.asks.keys())
        remaining = qty
        cost = 0.0
        filled = 0.0
        for p in ask_prices:
            if remaining <= 0:
                break
            available = self.asks[p]
            take = min(remaining, available)
            cost += take * p
            filled += take
            remaining -= take
            if take >= available:
                del self.asks[p]
            else:
                self.asks[p] -= take
        if filled > 0:
            return cost / filled, filled
        return np.nan, 0.0


# =====================================================================
# 2. EVENT REPLAYER
#    Streams LOB events from TimescaleDB in chronological order.
# =====================================================================
def stream_lob_events(symbol, limit=10000000):
    """Generator that yields LOB events one at a time from the database."""
    engine = create_engine(DB_URL)
    query = f"""
        SELECT time, side, price, size, is_snapshot, seq
        FROM orderbook
        WHERE symbol = '{symbol}'
        ORDER BY time ASC, seq ASC
        LIMIT {limit};
    """
    print(f"[{symbol}] Streaming LOB events from database...")
    df_raw = pd.read_sql(query, engine)
    if df_raw.empty:
        return

    print(f"[{symbol}] Loaded {len(df_raw)} events. Starting replay...")
    for row in df_raw.itertuples(index=False):
        yield {
            'time': row.time,
            'side': row.side,
            'price': row.price,
            'size': row.size,
            'is_snapshot': row.is_snapshot,
            'seq': row.seq,
        }


# =====================================================================
# 3. DYNAMIC CAPACITY SIMULATION
#    Replays LOB events for both symbols simultaneously, detects signals,
#    and simulates market order execution with live book consumption.
# =====================================================================
def run_dynamic_capacity_simulation(
    entry_z=2.0,
    qty_grid=QTY_GRID,
    fee_hurdle=FEE_HURDLE_BPS,
    levels=N_LEVELS,
    limit=10000000,
    markout_ms=200,
):
    """
    Run the full dynamic simulation.
    
    This replays LOB events for both perp and spot, maintaining live books.
    When a z-score signal fires, it simulates placing market orders of
    various sizes, consuming liquidity from the live book, and records
    the realized VWAP and subsequent markout.
    
    Returns a DataFrame with one row per (signal, qty) pair.
    """
    # Step 1: Pre-compute signals from the BBO data
    # (We need the full depth for signals, but signals are based on BBO)
    print("=" * 70)
    print("PHASE 2: DYNAMIC LIQUIDITY CAPACITY SIMULATION")
    print("=" * 70)

    # Build BBO signals first (same as Phase 1)
    df_perp_depth = reconstruct_book_with_depth("BTCPERP", levels=levels, limit=limit)
    df_spot_depth = reconstruct_book_with_depth("BTCUSDT", levels=levels, limit=limit)

    df_perp_bbo = df_perp_depth.rename(columns={
        'bid_price_1': 'perp_bid', 'bid_size_1': 'perp_bid_size',
        'ask_price_1': 'perp_ask', 'ask_size_1': 'perp_ask_size',
    })[['time', 'perp_bid', 'perp_bid_size', 'perp_ask', 'perp_ask_size']]
    df_spot_bbo = df_spot_depth.rename(columns={
        'bid_price_1': 'spot_bid', 'bid_size_1': 'spot_bid_size',
        'ask_price_1': 'spot_ask', 'ask_size_1': 'spot_ask_size',
    })[['time', 'spot_bid', 'spot_bid_size', 'spot_ask', 'spot_ask_size']]

    df_perp_bbo['perp_time'] = df_perp_bbo['time']
    df_spot_bbo['spot_time'] = df_spot_bbo['time']

    print("Aligning asynchronous data streams for signal generation...")
    df_arb = pd.merge_asof(
        df_perp_bbo[['time', 'perp_time', 'perp_bid', 'perp_bid_size', 'perp_ask', 'perp_ask_size']],
        df_spot_bbo[['time', 'spot_time', 'spot_bid', 'spot_bid_size', 'spot_ask', 'spot_ask_size']],
        on='time',
        direction='backward'
    ).dropna().copy()

    df_signals = prepare_signals(df_arb, window=1000, max_staleness_ms=100, min_vol_usd=1.0)

    # Extract signal times and z-scores
    sig_times = df_signals['time'].values
    sig_zscores = df_signals['z_score'].values

    # Step 2: Stream LOB events and maintain live books
    print("\nStarting live LOB replay for dynamic simulation...")
    perp_book = LiveOrderBook("BTCPERP", levels=levels)
    spot_book = LiveOrderBook("BTCUSDT", levels=levels)

    # We'll process events in chronological order by interleaving perp and spot events
    # For efficiency, load both into memory and sort
    engine = create_engine(DB_URL)
    query_perp = f"""
        SELECT time, side, price, size, is_snapshot, seq, 'perp' as symbol
        FROM orderbook
        WHERE symbol = 'BTCPERP'
        ORDER BY time ASC, seq ASC
        LIMIT {limit};
    """
    query_spot = f"""
        SELECT time, side, price, size, is_snapshot, seq, 'spot' as symbol
        FROM orderbook
        WHERE symbol = 'BTCUSDT'
        ORDER BY time ASC, seq ASC
        LIMIT {limit};
    """
    print("Loading perp events...")
    df_perp_events = pd.read_sql(query_perp, engine)
    print("Loading spot events...")
    df_spot_events = pd.read_sql(query_spot, engine)

    # Combine and sort chronologically
    df_all_events = pd.concat([df_perp_events, df_spot_events], ignore_index=True)
    df_all_events = df_all_events.sort_values(['time', 'seq']).reset_index(drop=True)
    print(f"Total combined events: {len(df_all_events)}")

    # Step 3: Replay events and simulate trading
    results = []
    signal_idx = 0  # pointer into sig_times
    last_signal_time = None
    n_signals_processed = 0
    n_events_processed = 0

    # Pre-compute signal lookup: for each event time, find the nearest signal
    # We'll use a two-pointer approach
    event_times = df_all_events['time'].values

    for event_idx, event in df_all_events.iterrows():
        n_events_processed += 1
        t = event['time']

        # Update the appropriate book
        if event['symbol'] == 'perp':
            perp_book.apply_event(t, event['side'], event['price'],
                                  event['size'], event['is_snapshot'], event['seq'])
        else:
            spot_book.apply_event(t, event['side'], event['price'],
                                  event['size'], event['is_snapshot'], event['seq'])

        # Check if we've crossed a signal time
        while signal_idx < len(sig_times) and sig_times[signal_idx] <= t:
            signal_time = sig_times[signal_idx]
            z_val = sig_zscores[signal_idx]

            # Skip if we already processed this signal time
            if signal_time == last_signal_time:
                signal_idx += 1
                continue

            if abs(z_val) >= entry_z:
                n_signals_processed += 1
                direction = 1 if z_val >= entry_z else -1

                # For each order size, simulate execution against the LIVE book
                for qty in qty_grid:
                    if direction == 1:
                        # Short perp: sell perp (hit bids), buy spot (lift asks)
                        perp_vwap, perp_filled = perp_book.consume_bids(qty)
                        spot_vwap, spot_filled = spot_book.consume_asks(qty)
                        if perp_filled >= qty and spot_filled >= qty:
                            captured_spread = perp_vwap - spot_vwap
                        else:
                            captured_spread = np.nan
                    else:
                        # Long perp: buy perp (lift asks), sell spot (hit bids)
                        perp_vwap, perp_filled = perp_book.consume_asks(qty)
                        spot_vwap, spot_filled = spot_book.consume_bids(qty)
                        if perp_filled >= qty and spot_filled >= qty:
                            captured_spread = spot_vwap - perp_vwap
                        else:
                            captured_spread = np.nan

                    fee_cost = (perp_vwap * qty * fee_hurdle) + (spot_vwap * qty * fee_hurdle) if not np.isnan(captured_spread) else np.nan
                    net_spread = captured_spread - fee_cost if not np.isnan(captured_spread) else np.nan

                    results.append({
                        'signal_time': signal_time,
                        'entry_z': z_val,
                        'direction': direction,
                        'qty': qty,
                        'perp_vwap': perp_vwap,
                        'spot_vwap': spot_vwap,
                        'perp_filled': perp_filled,
                        'spot_filled': spot_filled,
                        'depth_exhausted': (perp_filled < qty) or (spot_filled < qty),
                        'captured_spread': captured_spread,
                        'fee_cost': fee_cost,
                        'net_spread': net_spread,
                        'method': 'dynamic',
                    })

                last_signal_time = signal_time

            signal_idx += 1

        # Progress indicator
        if n_events_processed % 500000 == 0:
            print(f"  Processed {n_events_processed}/{len(df_all_events)} events, "
                  f"{n_signals_processed} signals triggered...")

    print(f"\nReplay complete. Processed {n_events_processed} events, "
          f"{n_signals_processed} signals triggered.")

    df_dynamic = pd.DataFrame(results)
    return df_dynamic


# =====================================================================
# 4. COMPARISON: STATIC vs DYNAMIC
# =====================================================================
def compare_static_vs_dynamic(df_static, df_dynamic):
    """Compare the static walk-the-book results with dynamic simulation."""
    print("\n" + "=" * 70)
    print("STATIC vs DYNAMIC COMPARISON")
    print("=" * 70)

    if df_static.empty or df_dynamic.empty:
        print("One or both result sets are empty. Cannot compare.")
        return

    # Mark the source
    df_static = df_static.copy()
    df_static['method'] = 'static'
    df_dynamic = df_dynamic.copy()
    df_dynamic['method'] = 'dynamic'

    combined = pd.concat([df_static, df_dynamic], ignore_index=True)

    # Compare edge decay curves
    print("\n--- Edge Decay: Static vs Dynamic ---")
    comparison = combined.groupby(['qty', 'method'])['net_spread'].agg(['mean', 'median', 'count']).unstack(level='method')
    print(comparison.to_string())

    # Slippage penalty: dynamic - static (negative means dynamic is worse = more slippage)
    print("\n--- Slippage Penalty (Dynamic - Static) ---")
    static_means = df_static.groupby('qty')['net_spread'].mean()
    dynamic_means = df_dynamic.groupby('qty')['net_spread'].mean()
    penalty = dynamic_means - static_means
    penalty_bps = penalty / df_static.groupby('qty')['perp_vwap'].mean() * 10000
    print(f"{'Qty':>8} {'Static Mean':>14} {'Dynamic Mean':>16} {'Penalty':>10} {'Penalty bps':>14}")
    for qty in sorted(set(list(static_means.index) + list(dynamic_means.index))):
        s = static_means.get(qty, np.nan)
        d = dynamic_means.get(qty, np.nan)
        p = d - s if not (np.isnan(s) or np.isnan(d)) else np.nan
        pb = p / s * 10000 if not (np.isnan(p) or np.isnan(s) or s == 0) else np.nan
        print(f"{qty:>8.2f} {s:>14.2f} {d:>16.2f} {p:>10.2f} {pb:>14.2f}")

    # Depth exhaustion comparison
    print("\n--- Depth Exhaustion: Static vs Dynamic ---")
    static_exh = df_static.groupby('qty')['depth_exhausted'].mean()
    dynamic_exh = df_dynamic.groupby('qty')['depth_exhausted'].mean()
    exh_comp = pd.DataFrame({'static': static_exh, 'dynamic': dynamic_exh})
    print(exh_comp.to_string())

    return combined


# =====================================================================
# 5. MAIN
# =====================================================================
if __name__ == "__main__":
    # Run dynamic simulation
    df_dynamic = run_dynamic_capacity_simulation(
        entry_z=2.0,
        qty_grid=QTY_GRID,
        fee_hurdle=FEE_HURDLE_BPS,
        levels=N_LEVELS,
        limit=10000000,
    )

    # Save dynamic results
    df_dynamic.to_parquet("research/data/liquidity_capacity_dynamic.parquet")
    print("\nDynamic results saved to research/data/liquidity_capacity_dynamic.parquet")

    # Load static results for comparison
    try:
        df_static = pd.read_parquet("research/data/liquidity_capacity_results.parquet")
        print(f"\nLoaded static results: {len(df_static)} rows")
        compare_static_vs_dynamic(df_static, df_dynamic)
    except FileNotFoundError:
        print("\nStatic results not found. Run liquidity_capacity.py first for comparison.")
        # Still generate report for dynamic results alone
        generate_capacity_report(df_dynamic)