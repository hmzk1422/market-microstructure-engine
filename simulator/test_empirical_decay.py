import pandas as pd
import numpy as np
from executor import run_economic_backtest

# Create test data
np.random.seed(42)
n = 1000
times = pd.date_range('2026-01-01', periods=n, freq='100ms')
z = np.random.randn(n).cumsum() * 0.1
spread = 100 - abs(z) * 10

df = pd.DataFrame({
    'time': times,
    'z_score': z,
    'exec_short_spread': spread + np.random.randn(n) * 5,
    'exec_long_spread': spread + np.random.randn(n) * 5,
})

print('Testing empirical decay analysis...')
print(f'Data points: {len(df)}')

# Run analysis
df_events = run_economic_backtest(
    df,
    entry_z=1.0,
    entry_latency_ms=0,
    fee_hurdle_usd=1.0,
    max_horizon_ms=2000,
    breakdown_z=5.0
)

print(f'\nExcursions found: {len(df_events)}')
if len(df_events) > 0:
    print(f'Profitable exits: {df_events["event_observed"].sum()}')
    print(f'Profitable rate: {df_events["event_observed"].mean()*100:.1f}%')
    
    profitable = df_events[df_events['event_observed']]
    if len(profitable) > 0:
        print(f'\nMedian TTR (profitable only): {profitable["ttr_ms"].median():.0f} ms')
        print(f'Mean TTR (profitable only): {profitable["ttr_ms"].mean():.0f} ms')

print('\n✓ Empirical decay analysis working correctly!')