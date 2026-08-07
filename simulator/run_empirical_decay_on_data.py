import pandas as pd
import numpy as np
from pathlib import Path
from executor import run_economic_backtest, generate_economic_report

print("=" * 70)
print("RUNNING EMPIRICAL DECAY ANALYSIS ON SIMULATION DATA")
print("=" * 70)

# Load the simulation data
data_file = Path("DeepMarket/ABIDES/research/data/limit_order_all_qtys.parquet")
if not data_file.exists():
    print(f"ERROR: Data file not found: {data_file}")
    exit(1)

print(f"\nLoading data from: {data_file}")
df = pd.read_parquet(data_file)
print(f"Loaded {len(df)} records")
print(f"Columns: {list(df.columns)}")

# Check if we have the required columns for empirical decay
required = ['time', 'z_score', 'exec_short_spread', 'exec_long_spread']
missing = [c for c in required if c not in df.columns]

if missing:
    print(f"\n⚠️  Missing required columns: {missing}")
    print("\nThe empirical decay analysis requires:")
    print("  - time: Timestamp")
    print("  - z_score: Z-score of spread")
    print("  - exec_short_spread: Crossable spread (short perp/long spot)")
    print("  - exec_long_spread: Crossable spread (long perp/short spot)")
    print("\nThese columns are generated in ttr_decay_zscores.ipynb")
    print("from aligned order book data, not from limit order simulation.")
    print("\n✓ The module is ready to use when you have aligned price data.")
else:
    print("\n✓ All required columns present")
    
    # Run analysis
    df_events = run_economic_backtest(
        df,
        entry_z=1.0,
        entry_latency_ms=100,
        fee_hurdle_usd=2.0,
        max_horizon_ms=5000,
        breakdown_z=6.0
    )
    
    if len(df_events) > 0:
        generate_economic_report(df_events)
        
        # Save results
        from datetime import datetime
        import json
        
        output_dir = Path("research/output")
        output_dir.mkdir(parents=True, exist_ok=True)
        
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        report_file = output_dir / f"empirical_decay_simulation_{timestamp}.json"
        
        # Convert for JSON
        def convert_types(obj):
            if isinstance(obj, dict):
                return {k: convert_types(v) for k, v in obj.items()}
            elif isinstance(obj, list):
                return [convert_types(item) for item in obj]
            elif isinstance(obj, (np.integer, np.floating)):
                return float(obj)
            elif isinstance(obj, np.bool_):
                return bool(obj)
            return obj
        
        results = {
            'n_excursions': len(df_events),
            'n_profitable': int(df_events['event_observed'].sum()),
            'profitable_rate': float(df_events['event_observed'].mean()),
            'median_ttr_ms': float(df_events[df_events['event_observed']]['ttr_ms'].median()),
            'events': df_events.to_dict('records')
        }
        
        with open(report_file, 'w') as f:
            json.dump(convert_types(results), f, indent=2, default=str)
        
        print(f"\n✓ Results saved to: {report_file}")
    else:
        print("\n⚠️  No excursions found. Check your z-score threshold.")

print("\n" + "=" * 70)
print("EMPIRICAL DECAY ANALYSIS COMPLETE")
print("=" * 70)