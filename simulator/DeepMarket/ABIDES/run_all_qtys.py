"""
run_all_qtys.py
===============
Helper script to run the ABIDES limit order simulation for each qty size
independently and combine the results.

Usage:
  cd simulator/DeepMarket/ABIDES
  python run_all_qtys.py
"""
import subprocess
import os
import glob
import pandas as pd

# Qty sizes to test
QTY_SIZES = [0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 25.0, 50.0, 100.0]

# Base config command
BASE_CMD = [
    "python", "config/crypto_limit_order_sim.py",
    "--event-limit", "100000"
]

# Output directory for individual runs
OUT_DIR = "research/data"
os.makedirs(OUT_DIR, exist_ok=True)

def main():
    print("=" * 70)
    print("RUNNING ALL QTY SIZES")
    print("=" * 70)
    
    for qty in QTY_SIZES:
        print(f"\n{'='*70}")
        print(f"Testing qty = {qty} BTC")
        print(f"{'='*70}")
        
        cmd = BASE_CMD + ["--qty", str(qty)]
        result = subprocess.run(cmd, check=False)
        
        if result.returncode != 0:
            print(f"WARNING: qty={qty} failed with return code {result.returncode}")
            continue
        
        # Move the parquet to a safe location before the next run
        src = "limit_order_abides_results.parquet"
        if os.path.exists(src):
            dst = os.path.join(OUT_DIR, f"limit_order_qty_{qty}.parquet")
            os.replace(src, dst)
            print(f"Saved results to {dst}")
        else:
            print(f"WARNING: No parquet file found after qty={qty} run")
    
    print("\n" + "=" * 70)
    print("COMBINING RESULTS")
    print("=" * 70)
    
    # Load all individual results
    files = [os.path.join(OUT_DIR, f"limit_order_qty_{qty}.parquet") 
             for qty in QTY_SIZES]
    files = [f for f in files if os.path.exists(f)]
    
    if not files:
        print("ERROR: No result files found!")
        return
    
    dfs = [pd.read_parquet(f) for f in files]
    df_all = pd.concat(dfs, ignore_index=True)
    
    combined_path = os.path.join(OUT_DIR, "limit_order_all_qtys.parquet")
    df_all.to_parquet(combined_path)
    print(f"Combined results saved to {combined_path}")
    print(f"Total rows: {len(df_all)}")
    
    # Print summary
    print("\n--- Combined Fill Rate by Order Size ---")
    summary = df_all.groupby('qty').agg(
        total=('both_filled', 'count'),
        filled=('both_filled', 'sum'),
        fill_rate=('both_filled', 'mean'),
        avg_net_spread=('net_spread', 'mean'),
        avg_ttf=('time_to_fill_sec', 'mean'),
    )
    print(summary)


if __name__ == "__main__":
    main()