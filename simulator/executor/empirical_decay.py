"""
empirical_decay.py
==================
Production-grade empirical decay rate analysis for arbitrage opportunities.

This module implements the economic Time-to-Profitable-Exit (TTR) analysis
that measures how long arbitrage opportunities last before decaying below
profitability thresholds.

Key Concepts:
--------------
- **TTR (Time-to-Profitable-Exit)**: Time from signal entry until the spread
  decays enough to be profitable (after fees). This is the TRUE empirical
  decay rate, not execution latency.

- **Kaplan-Meier Estimator**: Survival analysis that properly handles censored
  observations (breakdowns and timeouts) without survivorship bias.

- **Z-Score Tiers**: Analyzes decay rates at different signal strengths to
  answer: "At what z-score level do opportunities decay fastest?"

- **Exit Reasons**:
  - profit: Spread converged enough to be profitable
  - breakdown: Spread diverged further (|z| > breakdown_z)
  - timeout: Max horizon reached without profitable exit

Usage:
------
    from executor.empirical_decay import run_economic_backtest, kaplan_meier
    
    # Run analysis
    df_events = run_economic_backtest(
        df_signals,
        entry_z=1.0,
        fee_hurdle_usd=2.0,
        max_horizon_ms=5000,
        breakdown_z=6.0
    )
    
    # Generate Kaplan-Meier survival curve
    km = kaplan_meier(df_events['ttr_ms'].values, df_events['event_observed'].values)

References:
-----------
- Kaplan, E. L., & Meier, P. (1958). Nonparametric estimation from incomplete
  observations. Journal of the American Statistical Association.
"""

import numpy as np
import pandas as pd
from typing import Optional, Dict, List

# =====================================================================
# 2. ECONOMIC TTR ENGINE
#    TTR is redefined as: time from entry until the position could be
#    closed at a net profit (crossing-spread PnL minus fees), NOT time
#    until the z-score statistically reverts to 0. Entry_latency is placed
#    to show the profitability of the strategy, how fast the algorithm
#    must run.
#
#    Two failure modes are handled explicitly instead of being silently
#    dropped:
#      - breakdown: |z| keeps growing past a threshold -> the spread
#        relationship itself may have broken (non-arbitrageable regime).
#      - timeout: max_horizon_ms elapses with no profitable exit.
#    Both are recorded as *censored* observations (event=False), not
#    discarded, so downstream stats aren't survivorship-biased toward
#    only the trades that happened to work.
# =====================================================================
def run_economic_backtest(
    df,
    entry_z=1.0,
    entry_latency_ms=0,
    fee_hurdle_usd=20.0,
    max_horizon_ms=5000,
    breakdown_z=6.0,
):
    timestamps = df['time'].values
    z = df['z_score'].values
    exec_short = df['exec_short_spread'].values   # sell perp / buy spot
    exec_long = df['exec_long_spread'].values      # buy perp / sell spot
    n = len(df)

    events = []
    i = 0
    while i < n:
        if abs(z[i]) < entry_z:
            i += 1
            continue

        # --- entry (threshold crossing, not the unknown future extremum) ---
        signal_time = timestamps[i]
        excursion_type = 1 if z[i] >= entry_z else -1   # 1: short perp/long spot

        fill_idx = np.searchsorted(timestamps, signal_time + np.timedelta64(entry_latency_ms, 'ms'))
        fill_idx = min(fill_idx, n - 1)
        entry_fill = exec_short[fill_idx] if excursion_type == 1 else exec_long[fill_idx]

        entry_z_val = z[i]
        peak_z = abs(z[i])
        j = i + 1
        outcome = None  # ('profit', j) | ('breakdown', j) | ('timeout', j)

        while j < n:
            elapsed_ms = (timestamps[j] - signal_time) / np.timedelta64(1, 'ms')
            if abs(z[j]) > peak_z:
                peak_z = abs(z[j])

            # Mark-to-market net PnL if closed right now, using the
            # actually-crossable bid/ask spread, not mid.
            if excursion_type == 1:
                exit_pay = exec_long[j]           # buy back perp / sell spot
                net_pnl = entry_fill - exit_pay - fee_hurdle_usd
            else:
                exit_pay = exec_short[j]
                net_pnl = exit_pay - entry_fill - fee_hurdle_usd

            if net_pnl > 0:
                outcome = ('profit', j, net_pnl)
                break
            if abs(z[j]) > breakdown_z:
                outcome = ('breakdown', j, net_pnl)
                break
            if elapsed_ms > max_horizon_ms:
                outcome = ('timeout', j, net_pnl)
                break
            j += 1

        if outcome is None:
            # Data ran out before any resolution — still censor it,
            # do not drop it silently.
            j = n - 1
            net_pnl = (entry_fill - exec_long[j] - fee_hurdle_usd) if excursion_type == 1 \
                else (exec_short[j] - entry_fill - fee_hurdle_usd)
            outcome = ('timeout', j, net_pnl)

        reason, exit_idx, net_pnl = outcome
        ttr_ms = (timestamps[exit_idx] - signal_time) / np.timedelta64(1, 'ms')

        events.append({
            'entry_z': entry_z_val,
            'peak_z_score': peak_z,
            'excursion_type': excursion_type,
            'ttr_ms': ttr_ms,
            'net_pnl_usd': net_pnl,
            'exit_reason': reason,
            'event_observed': reason == 'profit',   # False => censored
        })

        i = exit_idx + 1

    return pd.DataFrame(events)


# =====================================================================
# 3. KAPLAN-MEIER ESTIMATOR
#    P(profitable exit achieved by time t), correctly accounting for
#    censored trades (breakdown / timeout) instead of just averaging
#    over the trades that happened to resolve.
# =====================================================================
def kaplan_meier(durations, event_observed):
    d = pd.DataFrame({'t': durations, 'event': event_observed}).sort_values('t')
    n_total = len(d)
    survival = 1.0
    rows = [(0.0, 1.0)]
    for t in sorted(d.loc[d['event'], 't'].unique()):
        at_risk = (d['t'] >= t).sum()
        died = ((d['t'] == t) & d['event']).sum()
        if at_risk > 0:
            survival *= (1 - died / at_risk)
        rows.append((t, survival))
    km = pd.DataFrame(rows, columns=['t_ms', 'survival_prob'])
    km['p_profitable_by_t'] = 1 - km['survival_prob']
    return km


# =====================================================================
# 4. REPORTING
# =====================================================================
def generate_economic_report(df_events):
    if len(df_events) == 0:
        print("No excursions identified.")
        return

    bins = [1.0, 2.0, 3.0, 4.0, np.inf]
    labels = ['1.0 - 2.0', '2.0 - 3.0', '3.0 - 4.0', '4.0+']
    df_events = df_events.copy()
    df_events['z_tier'] = pd.cut(df_events['peak_z_score'], bins=bins, labels=labels, right=False)

    print("\n" + "=" * 70)
    print("=== EVENT OUTCOME BREAKDOWN BY Z-TIER ===")
    print("=" * 70)
    outcome_tbl = df_events.groupby(['z_tier', 'exit_reason'], observed=False).size().unstack(fill_value=0)
    print(outcome_tbl.to_string())

    print("\n" + "=" * 70)
    print("=== TIME-TO-PROFITABLE-EXIT (ms) — OBSERVED EVENTS ONLY ===")
    print("=" * 70)
    profitable = df_events[df_events['event_observed']]
    ttr_stats = profitable.groupby('z_tier', observed=False)['ttr_ms'].agg(
        Count='count', p25=lambda x: np.percentile(x, 25) if len(x) else np.nan,
        Median='median', p75=lambda x: np.percentile(x, 75) if len(x) else np.nan,
    ).reset_index()
    print(ttr_stats.to_string(index=False))
    print("\nNote: this table alone survivorship-biases toward trades that")
    print("worked. Use the Kaplan-Meier curve below for the full picture,")
    print("including tiers dominated by breakdown/timeout censoring.")

    print("\n" + "=" * 70)
    print("=== KAPLAN-MEIER: P(profitable exit by time t), BY Z-TIER ===")
    print("=" * 70)
    for tier in labels:
        sub = df_events[df_events['z_tier'] == tier]
        if len(sub) == 0:
            continue
        km = kaplan_meier(sub['ttr_ms'].values, sub['event_observed'].values)
        censor_rate = 1 - sub['event_observed'].mean()
        print(f"\nz_tier {tier}  (n={len(sub)}, censored={censor_rate:.1%})")
        print(km.iloc[::max(1, len(km) // 8)].to_string(index=False))


# =====================================================================
# 5. EXECUTION
# =====================================================================
if __name__ == "__main__":
    df_signals = prepare_signals(df_arb, window=1000, max_staleness_ms=100, min_vol_usd=1.0)

    df_events = run_economic_backtest(
        df_signals,
        entry_z=1.0,
        entry_latency_ms=100,
        fee_hurdle_usd=2.0,
        max_horizon_ms=5000,   # tune to your actual holding tolerance
        breakdown_z=6.0,       # tune to how far you'll let z run before distrust
    )

    generate_economic_report(df_events)


def analyze_empirical_decay(
    df_signals: pd.DataFrame,
    entry_z: float = 1.0,
    entry_latency_ms: float = 100.0,
    fee_hurdle_usd: float = 2.0,
    max_horizon_ms: float = 5000.0,
    breakdown_z: float = 6.0,
    output_dir: str = "simulator/research/output"
) -> Dict:
    """
    Convenience function to run empirical decay analysis on simulation data.
    
    Args:
        df_signals: DataFrame with aligned price data
        entry_z: Minimum z-score threshold
        entry_latency_ms: Execution latency
        fee_hurdle_usd: Fee hurdle in USD
        max_horizon_ms: Maximum holding period
        breakdown_z: Breakdown z-score threshold
        output_dir: Directory to save reports
    
    Returns:
        Dictionary with analysis results
    """
    from pathlib import Path
    import json
    from datetime import datetime
    
    print("\n" + "=" * 70)
    print("EMPIRICAL DECAY ANALYSIS")
    print("=" * 70)
    print(f"Entry z-score: {entry_z}")
    print(f"Fee hurdle: ${fee_hurdle_usd:.2f}")
    print(f"Max horizon: {max_horizon_ms:.0f} ms")
    print(f"Breakdown z: {breakdown_z:.1f}")
    
    # Run backtest
    df_events = run_economic_backtest(
        df_signals,
        entry_z=entry_z,
        entry_latency_ms=entry_latency_ms,
        fee_hurdle_usd=fee_hurdle_usd,
        max_horizon_ms=max_horizon_ms,
        breakdown_z=breakdown_z,
    )
    
    if len(df_events) == 0:
        return {'error': 'No excursions found'}
    
    # Generate report
    generate_economic_report(df_events)
    
    # Compile results
    n_total = len(df_events)
    n_profitable = df_events['event_observed'].sum()
    
    results = {
        'n_total_excursions': int(n_total),
        'n_profitable': int(n_profitable),
        'n_censored': int(n_total - n_profitable),
        'profitable_rate': float(n_profitable / n_total),
        'events': df_events.to_dict('records'),
    }
    
    # Save report
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report_file = output_path / f"empirical_decay_report_{timestamp}.json"
    
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
    
    with open(report_file, 'w') as f:
        json.dump(convert_types(results), f, indent=2, default=str)
    
    print(f"\n[EmpiricalDecay] Report saved: {report_file}")
    print("=" * 70)
    
    return results