import pandas as pd
from sqlalchemy import create_engine

DB_URL = "postgresql://market:market@localhost:5433/market_data"

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
