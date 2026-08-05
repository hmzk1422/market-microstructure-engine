"""
crypto_limit_order_sim.py
==========================
ABIDES configuration for crypto limit order simulation.

Sets up a simulation with:
  1. ExchangeAgent handling both BTCPERP and BTCUSDT
  2. CryptoReplayAgent for each symbol (replays historical L2 data)
  3. ArbitrageLimitOrderAgent (places limit orders at z-score signals)

Commission: 0.02% per leg = 0.04% round-trip (limit orders)

Usage:
  cd simulator/DeepMarket/ABIDES
  python config/crypto_limit_order_sim.py
  python config/crypto_limit_order_sim.py --qty 0.5        # single qty test
  python config/crypto_limit_order_sim.py --event-limit 500000
"""

import sys
import os
import argparse
import numpy as np
import pandas as pd
from sqlalchemy import create_engine

# Add ABIDES to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from Kernel import Kernel
from util import util
from util.order.LimitOrder import LimitOrder
from agent.ExchangeAgent import ExchangeAgent
from agent.CryptoReplayAgent import CryptoReplayAgent
from agent.ArbitrageLimitOrderAgent import ArbitrageLimitOrderAgent

# Also add research directory for preprocessing imports
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', 'research'))

from preprocessing import prepare_signals, reconstruct_continuous_book
from liquidity_capacity import reconstruct_book_with_depth

DB_URL = "postgresql://market:market@localhost:5433/market_data"
PERP_SYMBOL = "BTCPERP"
SPOT_SYMBOL = "BTCUSDT"
EVENT_LIMIT = 100000
SEED = 42

# Full qty grid (in BTC)
QTY_GRID_BTC = [0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0]


def main():
    # Parse command-line arguments to allow running a single qty size per simulation.
    # This avoids the contamination problem where all order sizes are placed at once
    # and compete for the same queue. Run one qty per simulation for clean results.
    parser = argparse.ArgumentParser(description="ABIDES crypto limit order simulation")
    parser.add_argument("--qty", type=float, default=1.0,
                        help="Single order size in BTC to test (default: 1.0). "
                             "Use --all-qtys to run the full grid instead.")
    parser.add_argument("--all-qtys", action="store_true",
                        help="Run the full qty grid (WARNING: very slow, use run_all_qtys.py instead).")
    parser.add_argument("--event-limit", type=int, default=EVENT_LIMIT,
                        help="Number of LOB events to load per symbol.")
    args = parser.parse_args()

    # If --all-qtys is specified, use the full grid (not recommended - use run_all_qtys.py)
    if args.all_qtys:
        qty_grid = QTY_GRID_BTC
        print(f"WARNING: Running full qty grid: {qty_grid}")
        print(f"WARNING: This places ALL order sizes simultaneously at the same price level.")
        print(f"WARNING: This is extremely slow. Consider using 'python run_all_qtys.py' instead.")
    else:
        qty_grid = [args.qty]
        print(f"Running single-qty simulation: {args.qty} BTC")

    event_limit = args.event_limit

    np.random.seed(SEED)

    print("=" * 70)
    print("ABIDES CRYPTO LIMIT ORDER SIMULATION")
    print("=" * 70)
    print(f"Symbols: {PERP_SYMBOL}, {SPOT_SYMBOL}")
    print(f"Event limit: {event_limit} per symbol")
    print(f"Commission: 0.02% per leg = 0.04% round-trip")
    print()

    engine = create_engine(DB_URL)

    print("Determining market hours from data...")
    # IMPORTANT: The LIMIT must be applied per-symbol BEFORE aggregation.
    # A plain "SELECT MIN(time), MAX(time) ... LIMIT N" does NOT work because
    # the aggregate returns a single row, so LIMIT has no effect and MAX(time)
    # spans the ENTIRE database (causing the kernel to run for days).
    time_query = f"""
        SELECT MIN(t) as min_time, MAX(t) as max_time FROM (
            (SELECT time as t FROM orderbook
             WHERE symbol = '{PERP_SYMBOL}'
             ORDER BY time ASC, seq ASC
             LIMIT {event_limit})
            UNION ALL
            (SELECT time as t FROM orderbook
             WHERE symbol = '{SPOT_SYMBOL}'
             ORDER BY time ASC, seq ASC
             LIMIT {event_limit})
        ) sub;
    """
    time_df = pd.read_sql(time_query, engine)

    if time_df.empty or time_df['min_time'].iloc[0] is None:
        print("ERROR: No data found in database!")
        return

    mkt_open = pd.to_datetime(time_df['min_time'].iloc[0])
    mkt_close = pd.to_datetime(time_df['max_time'].iloc[0])

    if hasattr(mkt_open, 'tzinfo') and mkt_open.tzinfo is not None:
        mkt_open = mkt_open.tz_localize(None)
    if hasattr(mkt_close, 'tzinfo') and mkt_close.tzinfo is not None:
        mkt_close = mkt_close.tz_localize(None)

    mkt_open = mkt_open - pd.Timedelta(seconds=1)
    mkt_close = mkt_close + pd.Timedelta(seconds=10)

    print(f"Market open: {mkt_open}")
    print(f"Market close: {mkt_close}")

    print("\nPre-computing z-score signals...")
    df_perp_depth = reconstruct_book_with_depth(PERP_SYMBOL, levels=10, limit=event_limit)
    df_spot_depth = reconstruct_book_with_depth(SPOT_SYMBOL, levels=10, limit=event_limit)

    if hasattr(df_perp_depth['time'], 'dt') and df_perp_depth['time'].dt.tz is not None:
        df_perp_depth['time'] = df_perp_depth['time'].dt.tz_localize(None)
    if hasattr(df_spot_depth['time'], 'dt') and df_spot_depth['time'].dt.tz is not None:
        df_spot_depth['time'] = df_spot_depth['time'].dt.tz_localize(None)

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
        on='time', direction='backward'
    ).dropna().copy()

    df_signals = prepare_signals(df_arb, window=1000, max_staleness_ms=100, min_vol_usd=1.0)

    if hasattr(df_signals['time'], 'dt') and df_signals['time'].dt.tz is not None:
        df_signals['time'] = df_signals['time'].dt.tz_localize(None)

    n_signals = len(df_signals[abs(df_signals['z_score']) >= 2.0])
    print(f"Generated {len(df_signals)} signals total, {n_signals} above |z| >= 2.0")

    print("\nCreating ABIDES agents...")
    agents = []
    agent_types = []

    print("  Creating ExchangeAgent...")
    exchange_agent = ExchangeAgent(
        id=0, name="EXCHANGE_AGENT", type="ExchangeAgent",
        mkt_open=mkt_open, mkt_close=mkt_close,
        symbols=[PERP_SYMBOL, SPOT_SYMBOL],
        log_orders=False,
        pipeline_delay=0,
        computation_delay=0,
        stream_history=0,
        book_freq=None,
        random_state=np.random.RandomState(seed=SEED),
        log_to_file=False
    )
    agents.append(exchange_agent)
    agent_types.append("ExchangeAgent")

    print(f"  Creating CryptoReplayAgent for {PERP_SYMBOL}...")
    perp_replay = CryptoReplayAgent(
        id=1, name="PERP_REPLAY", type="CryptoReplayAgent",
        symbol=PERP_SYMBOL,
        start_time=mkt_open, end_time=mkt_close,
        starting_cash=0,
        log_orders=False,
        log_to_file=False,
        random_state=np.random.RandomState(seed=SEED+1),
        limit=event_limit
    )
    agents.append(perp_replay)
    agent_types.append("CryptoReplayAgent")

    print(f"  Creating CryptoReplayAgent for {SPOT_SYMBOL}...")
    spot_replay = CryptoReplayAgent(
        id=2, name="SPOT_REPLAY", type="CryptoReplayAgent",
        symbol=SPOT_SYMBOL,
        start_time=mkt_open, end_time=mkt_close,
        starting_cash=0,
        log_orders=False,
        log_to_file=False,
        random_state=np.random.RandomState(seed=SEED+2),
        limit=event_limit
    )
    agents.append(spot_replay)
    agent_types.append("CryptoReplayAgent")

    print("  Creating ArbitrageLimitOrderAgent...")
    arb_agent = ArbitrageLimitOrderAgent(
        id=3, name="ARB_AGENT", type="ArbitrageLimitOrderAgent",
        perp_symbol=PERP_SYMBOL, spot_symbol=SPOT_SYMBOL,
        signals=df_signals,
        starting_cash=10_000_000_000,
        log_orders=False,
        log_to_file=False,
        random_state=np.random.RandomState(seed=SEED+3),
        entry_z=2.0,
        qty_grid_btc=qty_grid,
        max_wait_sec=5.0,
        fee_bps=0.0002
    )
    agents.append(arb_agent)
    agent_types.append("ArbitrageLimitOrderAgent")

    print(f"\nTotal agents: {len(agents)}")
    print(f"Agent types: {agent_types}")

    print("\nConfiguring ABIDES Kernel...")
    kernel = Kernel("Crypto Limit Order Kernel",
                    random_state=np.random.RandomState(seed=SEED))

    kernelStartTime = mkt_open
    kernelStopTime = mkt_close + pd.Timedelta(seconds=30)

    defaultComputationDelay = 0
    latency = np.zeros((len(agents), len(agents)))
    noise = [0.0]

    print(f"Kernel start: {kernelStartTime}")
    print(f"Kernel stop: {kernelStopTime}")

    print("\nPre-loading replay agent events...")
    perp_replay._load_events()
    spot_replay._load_events()

    print("Pre-loading arbitrage agent signals...")
    arb_agent._prepare_signals()

    print("\n" + "=" * 70)
    print("STARTING ABIDES SIMULATION")
    print("=" * 70)

    import datetime as dt
    sim_start = dt.datetime.now()

    kernel.runner(
        agents=agents,
        startTime=kernelStartTime,
        stopTime=kernelStopTime,
        agentLatency=latency,
        latencyNoise=noise,
        defaultComputationDelay=defaultComputationDelay,
        defaultLatency=0,
        oracle=None,
        log_dir=f"abides_log_{sim_start.strftime('%Y%m%d_%H%M%S')}"
    )

    sim_end = dt.datetime.now()
    print(f"\n{'=' * 70}")
    print(f"SIMULATION COMPLETE")
    print(f"{'=' * 70}")
    print(f"Total time: {sim_end - sim_start}")


if __name__ == "__main__":
    main()