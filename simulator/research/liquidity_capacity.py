"""
liquidity_capacity.py
=====================
Phase 1: Static Liquidity Capacity Curve (Walk-the-Book)

Measures how much size can be executed on each leg (perp + spot) before
the captured spread decays below the fee hurdle.

Fee hurdle assumptions (from user):
  - Limit order: 0.02% per leg → 0.04% round-trip
  - Market order: 0.05% per leg → 0.10% round-trip
  - We use 0.10% (10 bps) as the hurdle since we're measuring market-order capacity.
"""

import numpy as np
import pandas as pd
from sqlalchemy import create_engine
from simulator.research.preprocessing import prepare_signals, reconstruct_continuous_book

DB_URL = "postgresql://market:market@localhost:5433/market_data"

# Fee hurdle: 0.10% = 10 bps for market-order round-trip
FEE_HURDLE_BPS = 0.0010

# Order size grid (in BTC units)
QTY_GRID = np.array([0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0])

# Depth levels to walk
N_LEVELS = 10


# =====================================================================
# 1. FULL-DEPTH RECONSTRUCTION (TOP-N LEVELS)
#    Extended from preprocessing.reconstruct_continuous_book to preserve
#    multiple price levels on each side.
# =====================================================================
def reconstruct_book_with_depth(symbol, levels=N_LEVELS, limit=10000000):
    """Reconstruct top-N levels of the order book from raw LOB events."""
    engine = create_engine(DB_URL)
    query = f"""
        SELECT time, side, price, size, is_snapshot, seq
        FROM orderbook
        WHERE symbol = '{symbol}'
        ORDER BY time ASC, seq ASC
        LIMIT {limit};
    """
    print(f"[{symbol}] Fetching raw LOB events from database...")
    df_raw = pd.read_sql(query, engine)
    if df_raw.empty:
        return pd.DataFrame()

    print(f"[{symbol}] Reconstructing top-{levels} depth from {len(df_raw)} events...")
    bids, asks = {}, {}
    timeline = []
    current_time = df_raw['time'].iloc[0]
    current_seq = df_raw['seq'].iloc[0]

    def snapshot_levels():
        bid_prices = sorted(bids.keys(), reverse=True)[:levels]
        ask_prices = sorted(asks.keys())[:levels]
        row = {'time': current_time}
        for k in range(levels):
            row[f'bid_price_{k+1}'] = bid_prices[k] if k < len(bid_prices) else np.nan
            row[f'bid_size_{k+1}']  = bids.get(bid_prices[k], np.nan) if k < len(bid_prices) else np.nan
            row[f'ask_price_{k+1}'] = ask_prices[k] if k < len(ask_prices) else np.nan
            row[f'ask_size_{k+1}']  = asks.get(ask_prices[k], np.nan) if k < len(ask_prices) else np.nan
        return row

    for row in df_raw.itertuples(index=False):
        if row.time != current_time or row.seq != current_seq:
            if bids and asks:
                timeline.append(snapshot_levels())
            current_time = row.time
            current_seq = row.seq

        if row.is_snapshot:
            (bids if row.side == 'bid' else asks).clear()

        book_side = bids if row.side == 'bid' else asks
        if row.size == 0:
            book_side.pop(row.price, None)
        else:
            book_side[row.price] = row.size

    if bids and asks:
        timeline.append(snapshot_levels())

    df_depth = pd.DataFrame(timeline)
    df_depth['time'] = pd.to_datetime(df_depth['time'])
    df_depth = df_depth.sort_values('time').reset_index(drop=True)
    print(f"[{symbol}] Generated {len(df_depth)} depth snapshots (top {levels} levels).\n")
    return df_depth


# =====================================================================
# 2. WALK-THE-BOOK VWAP
#    Given a sorted list of (price, size) levels, compute the VWAP and
#    filled quantity for a target order size.
# =====================================================================
def walk_book_vwap(prices, sizes, qty):
    """
    Walk the book from best to worst, consuming liquidity.
    
    Parameters
    ----------
    prices : array-like of float
        Price levels, sorted from best to worst (e.g., ascending for asks,
        descending for bids).
    sizes : array-like of float
        Available size at each price level.
    qty : float
        Target order quantity.
    
    Returns
    -------
    vwap : float or NaN
        Volume-weighted average price if fully filled, NaN if depth exhausted.
    filled : float
        Total quantity filled (may be < qty if depth exhausted).
    """
    remaining = qty
    cost = 0.0
    filled = 0.0
    for p, s in zip(prices, sizes):
        if remaining <= 0 or pd.isna(p) or pd.isna(s):
            break
        take = min(remaining, s)
        cost += take * p
        filled += take
        remaining -= take
    if filled > 0:
        return cost / filled, filled
    return np.nan, 0.0


# =====================================================================
# 3. LIQUIDITY CAPACITY PER SIGNAL
#    For each z-score excursion, compute the captured spread (net of
#    slippage) for each order size in the grid.
# =====================================================================
# =====================================================================
# 3. LIQUIDITY CAPACITY PER SIGNAL (FIXED)
#    For each z-score excursion, compute the captured spread (net of
#    slippage) for each order size in the grid.
# =====================================================================
def compute_capacity_per_signal(
    df_depth_perp,
    df_depth_spot,
    df_signals,
    entry_z=2.0,
    qty_grid=QTY_GRID,
    fee_hurdle=FEE_HURDLE_BPS,
    levels=N_LEVELS,
):
    """
    For each signal (z-score excursion), walk the book on both legs and
    compute the net captured spread after slippage and fees.
    
    Returns a DataFrame with one row per (signal, qty) pair.
    """
    sig_time = df_signals['time'].values
    z = df_signals['z_score'].values
    dt_perp = df_depth_perp['time'].values
    dt_spot = df_depth_spot['time'].values

    # Column names for the depth DataFrame
    bpc = [f'bid_price_{k+1}' for k in range(levels)]
    bsc = [f'bid_size_{k+1}' for k in range(levels)]
    apc = [f'ask_price_{k+1}' for k in range(levels)]
    asc = [f'ask_size_{k+1}' for k in range(levels)]

    rows = []
    n_signals = 0

    for i in range(len(df_signals)):
        if abs(z[i]) < entry_z:
            continue
        n_signals += 1
        direction = 1 if z[i] >= entry_z else -1  # 1 = short perp / long spot
        t = sig_time[i]

        # Find the nearest depth snapshot (backward-looking)
        perp_idx = min(np.searchsorted(dt_perp, t, side='right') - 1, len(df_depth_perp) - 1)
        spot_idx = min(np.searchsorted(dt_spot, t, side='right') - 1, len(df_depth_spot) - 1)
        if perp_idx < 0 or spot_idx < 0:
            continue

        prow = df_depth_perp.iloc[perp_idx]
        srow = df_depth_spot.iloc[spot_idx]

        for qty in qty_grid:
            if direction == 1:
                # Short perp: sell perp (hit bid side), buy spot (pay ask side)
                perp_vwap, perp_filled = walk_book_vwap(
                    prow[bpc].values, prow[bsc].values, qty
                )
                spot_vwap, spot_filled = walk_book_vwap(
                    srow[apc].values, srow[asc].values, qty
                )
                # Captured spread = perp sold price - spot bought price
                if perp_filled >= qty and spot_filled >= qty:
                    captured_spread = perp_vwap - spot_vwap
                else:
                    captured_spread = np.nan
            else:
                # Long perp: buy perp (pay ask side), sell spot (hit bid side)
                perp_vwap, perp_filled = walk_book_vwap(
                    prow[apc].values, prow[asc].values, qty
                )
                spot_vwap, spot_filled = walk_book_vwap(
                    srow[bpc].values, srow[bsc].values, qty
                )
                # Captured spread = spot sold price - perp bought price
                if perp_filled >= qty and spot_filled >= qty:
                    captured_spread = spot_vwap - perp_vwap
                else:
                    captured_spread = np.nan

            # [FIXED SECTION] Ensure all calculations are PER-UNIT (per 1 BTC)
            # Calculate the fee per 1 BTC executed
            per_unit_fee = (perp_vwap * fee_hurdle) + (spot_vwap * fee_hurdle) if not np.isnan(captured_spread) else np.nan
            
            # Net spread per 1 BTC
            net_spread = captured_spread - per_unit_fee if not np.isnan(captured_spread) else np.nan
            
            # (Optional) Calculate total dollar profit for the whole trade size
            total_net_profit = net_spread * qty if not np.isnan(net_spread) else np.nan

            rows.append({
                'signal_time': t,
                'entry_z': z[i],
                'direction': direction,
                'qty': qty,
                'perp_vwap': perp_vwap,
                'spot_vwap': spot_vwap,
                'perp_filled': perp_filled,
                'spot_filled': spot_filled,
                'depth_exhausted': (perp_filled < qty) or (spot_filled < qty),
                'captured_spread': captured_spread,
                'fee_cost_per_unit': per_unit_fee,
                'net_spread': net_spread, 
                'total_net_profit': total_net_profit
            })

    df_cap = pd.DataFrame(rows)
    print(f"Processed {n_signals} signals across {len(qty_grid)} size buckets = {len(df_cap)} rows.")
    return df_cap

# =====================================================================
# 4. AGGREGATION & REPORTING
# =====================================================================
def generate_capacity_report(df_cap, fee_hurdle_bps=FEE_HURDLE_BPS):
    """Generate summary statistics and edge decay curves."""
    if df_cap.empty:
        print("No capacity data to report.")
        return

    print("\n" + "=" * 70)
    print("=== LIQUIDITY CAPACITY REPORT ===")
    print(f"Fee hurdle: {fee_hurdle_bps*10000:.1f} bps per leg")
    print("=" * 70)

    # 1. Edge decay curve: mean/median net spread by order size
    decay = df_cap.groupby('qty')['net_spread'].agg(['mean', 'median', 'std', 'count'])
    decay['mean_bps'] = decay['mean'] / df_cap.groupby('qty')['perp_vwap'].mean() * 10000
    decay['median_bps'] = decay['median'] / df_cap.groupby('qty')['perp_vwap'].mean() * 10000
    print("\n--- Edge Decay Curve (Net Spread by Order Size) ---")
    print(f"{'Qty':>8} {'Count':>8} {'Mean Spread':>14} {'Median Spread':>16} {'Mean bps':>10} {'Median bps':>12}")
    for qty, row in decay.iterrows():
        print(f"{qty:>8.2f} {row['count']:>8.0f} {row['mean']:>14.2f} {row['median']:>16.2f} {row['mean_bps']:>10.2f} {row['median_bps']:>12.2f}")

    # 2. Depth exhaustion probability
    exhaustion = df_cap.groupby('qty')['depth_exhausted'].mean()
    print("\n--- Depth Exhaustion Probability ---")
    print(f"{'Qty':>8} {'P(Exhausted)':>16}")
    for qty, prob in exhaustion.items():
        print(f"{qty:>8.2f} {prob:>16.2%}")

    # 3. Maximum safe size (where net spread > 0 for at least 50% of signals)
    safe_sizes = []
    for qty in sorted(df_cap['qty'].unique()):
        sub = df_cap[df_cap['qty'] == qty].dropna(subset=['net_spread'])
        if len(sub) == 0:
            continue
        p_positive = (sub['net_spread'] > 0).mean()
        if p_positive >= 0.50:
            safe_sizes.append((qty, p_positive, sub['net_spread'].median()))
    if safe_sizes:
        print("\n--- Maximum Safe Order Sizes (P(net spread > 0) >= 50%) ---")
        print(f"{'Qty':>8} {'P(Positive)':>14} {'Median Net Spread':>20}")
        for qty, p_pos, med in safe_sizes:
            print(f"{qty:>8.2f} {p_pos:>14.2%} {med:>20.2f}")
        max_safe = safe_sizes[-1][0]
        print(f"\n=> Maximum recommended order size: {max_safe:.2f} BTC")
    else:
        print("\n=> No order size has P(net spread > 0) >= 50%")

    # 4. Capacity distribution: for each signal, find the max qty where net spread > 0
    print("\n--- Per-Signal Capacity Distribution ---")
    signal_capacity = []
    for sig_time in df_cap['signal_time'].unique():
        sub = df_cap[df_cap['signal_time'] == sig_time].dropna(subset=['net_spread'])
        if len(sub) == 0:
            continue
        positive = sub[sub['net_spread'] > 0]
        if len(positive) > 0:
            max_qty = positive['qty'].max()
            signal_capacity.append(max_qty)
    if signal_capacity:
        caps = pd.Series(signal_capacity)
        print(f"Mean capacity:  {caps.mean():.2f} BTC")
        print(f"Median capacity: {caps.median():.2f} BTC")
        print(f"P25 capacity:   {caps.quantile(0.25):.2f} BTC")
        print(f"P75 capacity:   {caps.quantile(0.75):.2f} BTC")
        print(f"Max capacity:   {caps.max():.2f} BTC")
        print(f"Min capacity:   {caps.min():.2f} BTC")
    else:
        print("No signals with positive net spread found.")

    return decay, exhaustion, signal_capacity


# =====================================================================
# 5. MAIN
# =====================================================================
if __name__ == "__main__":
    print("=" * 70)
    print("PHASE 1: STATIC LIQUIDITY CAPACITY ANALYSIS")
    print("=" * 70)

    # Step 1: Reconstruct full depth for both symbols
    df_perp_depth = reconstruct_book_with_depth("BTCPERP", levels=N_LEVELS)
    df_spot_depth = reconstruct_book_with_depth("BTCUSDT", levels=N_LEVELS)

    # Step 2: Build BBO-level signals (reuse existing pipeline)
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

    print("Aligning asynchronous data streams...")
    df_arb = pd.merge_asof(
        df_perp_bbo[['time', 'perp_time', 'perp_bid', 'perp_bid_size', 'perp_ask', 'perp_ask_size']],
        df_spot_bbo[['time', 'spot_time', 'spot_bid', 'spot_bid_size', 'spot_ask', 'spot_ask_size']],
        on='time',
        direction='backward'
    ).dropna().copy()

    # Step 3: Generate signals
    df_signals = prepare_signals(df_arb, window=1000, max_staleness_ms=100, min_vol_usd=1.0)

    # Step 4: Compute capacity
    df_capacity = compute_capacity_per_signal(
        df_perp_depth, df_spot_depth, df_signals,
        entry_z=2.0,
        qty_grid=QTY_GRID,
        fee_hurdle=FEE_HURDLE_BPS,
    )

    # Step 5: Report
    decay, exhaustion, signal_capacity = generate_capacity_report(df_capacity)

    # Step 6: Save results
    df_capacity.to_parquet("research/data/liquidity_capacity_results.parquet")
    print("\nResults saved to research/data/liquidity_capacity_results.parquet")

"""
Explanation:

With the constructed book, we utilized walk-by-VWAP system where we clear 10 levels of bid ask whenever available
and does so on every data point. This is a naive assumption using market orders.
- Results show that market orders are very inefficient for arbitraging where we can only profitably trade on 0.10BTC.
-  

"""