import pandas as pd
from sqlalchemy import create_engine
import numpy as np

DB_URL = "postgresql://market:market@localhost:5433/market_data"

# =====================================================================
# 1. DATA INGESTION & SIGNAL CONSTRUCTION
#    (unchanged from the existing pipeline — kept here for completeness)
# =====================================================================

def prepare_signals(df, window=1000, max_staleness_ms=100, min_vol_usd=1.0):
    df = df.copy()
    df['feed_latency_ms'] = (df['perp_time'] - df['spot_time']).abs().dt.total_seconds() * 1000.0
    df['is_stale'] = df['feed_latency_ms'] > max_staleness_ms

    df['perp_mid'] = (df['perp_bid'] + df['perp_ask']) / 2.0
    df['spot_mid'] = (df['spot_bid'] + df['spot_ask']) / 2.0
    df['mid_spread'] = df['perp_mid'] - df['spot_mid']

    df['exec_short_spread'] = df['perp_bid'] - df['spot_ask']   # sell perp / buy spot
    df['exec_long_spread'] = df['perp_ask'] - df['spot_bid']    # buy perp / sell spot

    rolling_med = df['mid_spread'].rolling(window=window, min_periods=window // 2).median()
    rolling_mad = (df['mid_spread'] - rolling_med).abs().rolling(window=window, min_periods=window // 2).median()
    scale = np.maximum(rolling_mad * 1.4826, min_vol_usd)
    df['z_score'] = (df['mid_spread'] - rolling_med) / scale
    df.loc[df['is_stale'], 'z_score'] = 0.0
    return df


def reconstruct_continuous_book(symbol, limit=10000000):
    engine = create_engine(DB_URL)

    # Fetch the raw order book events, strictly ordered by time and sequence
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

    print(f"[{symbol}] Reconstructing book from {len(df_raw)} events...")
    
    bids = {}
    asks = {}
    bbo_timeline = []

    current_time = df_raw['time'].iloc[0]
    current_seq = df_raw['seq'].iloc[0]

    # Fast iteration using itertuples instead of groupby().iterrows()
    for row in df_raw.itertuples(index=False):
        
        if row.time != current_time or row.seq != current_seq:
            best_bid = max(bids.keys()) if bids else None
            best_ask = min(asks.keys()) if asks else None
            
            if best_bid and best_ask:
                bbo_timeline.append({
                    'time': current_time,
                    'best_bid': best_bid,
                    'best_ask': best_ask,
                    'spread': best_ask - best_bid
                })
            
            current_time = row.time
            current_seq = row.seq
            
        if row.is_snapshot:
            if row.side == 'bid':
                bids.clear()
            else:
                asks.clear()

        if row.side == 'bid':
            if row.size == 0:
                bids.pop(row.price, None)
            else:
                bids[row.price] = row.size
        else:
            if row.size == 0:
                asks.pop(row.price, None)
            else:
                asks[row.price] = row.size

    # Catch the final state
    best_bid = max(bids.keys()) if bids else None
    best_ask = min(asks.keys()) if asks else None
    if best_bid and best_ask:
        bbo_timeline.append({
            'time': current_time,
            'best_bid': best_bid,
            'best_ask': best_ask,
            'spread': best_ask - best_bid
        })

    df_bbo = pd.DataFrame(bbo_timeline)
    
    # Ensure time is a proper datetime object and sorted for the asof join
    df_bbo['time'] = pd.to_datetime(df_bbo['time'])
    df_bbo = df_bbo.sort_values('time')
    
    print(f"[{symbol}] Successfully generated {len(df_bbo)} continuous BBO states.\n")
    return df_bbo
