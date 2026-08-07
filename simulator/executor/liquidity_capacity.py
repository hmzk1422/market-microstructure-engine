"""
liquidity_capacity.py
=====================
Capital capacity and slippage analysis for cross-exchange arbitrage.

Measures effective liquidity depth and how execution size impacts fills:
- Non-linear market impact curves
- Phantom liquidity detection (spoofing, layering)
- Order book resilience analysis
- Slippage modeling by order size
- Capital capacity curves (max deployable capital)

References:
    "Market Depth and Order Size" - Foucault, Pagano, Röell (2013)
    "Liquidity, Information, and the Impermanence of Financial Markets" - Kyle (1985)
"""

import numpy as np
import pandas as pd
from scipy.optimize import curve_fit
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple
import warnings


@dataclass
class LiquiditySnapshot:
    """Point-in-time liquidity measurement."""
    timestamp: pd.Timestamp
    symbol: str
    best_bid: float
    best_ask: float
    bid_size_levels: np.ndarray  # sizes at bid levels
    ask_size_levels: np.ndarray  # sizes at ask levels
    bid_prices: np.ndarray
    ask_prices: np.ndarray
    mid_price: float
    spread_bps: float
    
    @property
    def total_bid_depth(self) -> float:
        """Total liquidity on bid side (size * price)."""
        return np.sum(self.bid_size_levels * self.bid_prices)
    
    @property
    def total_ask_depth(self) -> float:
        """Total liquidity on ask side."""
        return np.sum(self.ask_size_levels * self.ask_prices)
    
    @property
    def imbalance_ratio(self) -> float:
        """Bid/ask depth imbalance. >1 = bid heavy, <1 = ask heavy."""
        ask_depth = self.total_ask_depth
        if ask_depth == 0:
            return np.inf
        return self.total_bid_depth / ask_depth


class LiquidityCapacityAnalyzer:
    """
    Analyzes liquidity capacity and market impact for arbitrage execution.
    
    Key metrics:
        1. Visible depth at best levels
        2. Slippage curve (execution price vs order size)
        3. Capacity limit (max size before spread > fee hurdle)
        4. Phantom liquidity ratio (top 3 levels vs total depth)
    """
    
    def __init__(self):
        self.liquidity_snapshots: List[LiquiditySnapshot] = []
        self.slippage_curves: Dict[str, pd.DataFrame] = {}
    
    def add_snapshot(self, snapshot: LiquiditySnapshot):
        """Add a liquidity snapshot for analysis."""
        self.liquidity_snapshots.append(snapshot)
    
    def calculate_slippage(self, order_size_btc: float,
                           side: str,
                           snapshot: LiquiditySnapshot) -> Dict[str, float]:
        """
        Calculate expected slippage for a market order.
        
        Args:
            order_size_btc: Size of market order in BTC
            side: 'buy' or 'sell'
            snapshot: LiquiditySnapshot at time of execution
        
        Returns:
            Dict with fill price, slippage, and effective price
        """
        if side == 'buy':
            prices = snapshot.ask_prices
            sizes = snapshot.ask_size_levels
            best_price = snapshot.best_ask
        else:
            prices = snapshot.bid_prices
            sizes = snapshot.bid_size_levels
            best_price = snapshot.best_bid
        
        order_size_sat = order_size_btc * 100_000_000
        
        remaining = order_size_sat
        total_cost = 0.0
        levels_consumed = 0
        
        for price, size in zip(prices, sizes):
            if remaining <= 0:
                break
            fill_size = min(remaining, size)
            total_cost += fill_size * price
            remaining -= fill_size
            levels_consumed += 1
        
        if remaining > 0:
            # Order not fully fillable at current depth
            fill_rate = 1.0 - (remaining / order_size_sat)
            avg_price = total_cost / (order_size_sat - remaining) if (order_size_sat - remaining) > 0 else best_price
        else:
            fill_rate = 1.0
            avg_price = total_cost / order_size_sat
        
        slippage = avg_price - best_price if side == 'buy' else best_price - avg_price
        slippage_bps = (slippage / best_price) * 10000 if best_price > 0 else 0
        
        return {
            'order_size_btc': order_size_btc,
            'side': side,
            'best_price': best_price,
            'avg_fill_price': avg_price,
            'slippage': slippage,
            'slippage_bps': slippage_bps,
            'fill_rate': fill_rate,
            'levels_consumed': levels_consumed,
            'total_cost': total_cost,
        }
    
    def fit_slippage_curve(self, order_sizes: np.ndarray,
                            slippage_bps: np.ndarray) -> Dict[str, float]:
        """
        Fit power-law slippage curve: slippage = a * size^b
        
        Args:
            order_sizes: Array of order sizes
            slippage_bps: Array of corresponding slippage in bps
        
        Returns:
            Dict with fitted parameters
        """
        # Filter valid data
        mask = (order_sizes > 0) & (slippage_bps > 0) & np.isfinite(order_sizes) & np.isfinite(slippage_bps)
        x = order_sizes[mask]
        y = slippage_bps[mask]
        
        if len(x) < 3:
            return {'a': np.nan, 'b': np.nan, 'r_squared': 0.0}
        
        # Log-linear fit: log(slippage) = log(a) + b * log(size)
        log_x = np.log(x)
        log_y = np.log(y)
        
        # Linear regression
        coeffs = np.polyfit(log_x, log_y, 1)
        b = coeffs[0]
        log_a = coeffs[1]
        a = np.exp(log_a)
        
        # R-squared
        y_pred = np.exp(log_a + b * log_x)
        ss_res = np.sum((y - y_pred) ** 2)
        ss_tot = np.sum((y - np.mean(y)) ** 2)
        r_squared = 1 - (ss_res / ss_tot) if ss_tot > 0 else 0.0
        
        return {
            'a': a,
            'b': b,  # Exponent: >1 = superlinear (thin market), <1 = sublinear (deep market)
            'r_squared': r_squared,
            'n_samples': len(x),
        }
    
    def calculate_capacity_limit(self, fee_hurdle_bps: float,
                                  snapshot: LiquiditySnapshot,
                                  side: str = 'both',
                                  max_depth_levels: int = 20) -> Dict[str, float]:
        """
        Calculate maximum executable size before slippage exceeds fee hurdle.
        
        Args:
            fee_hurdle_bps: The minimum spread needed to be profitable
            snapshot: Current liquidity snapshot
            side: 'buy', 'sell', or 'both'
            max_depth_levels: How many levels to walk
        
        Returns:
            Dict with capacity metrics
        """
        if side == 'buy':
            prices = snapshot.ask_prices[:max_depth_levels]
            sizes = snapshot.ask_size_levels[:max_depth_levels]
        elif side == 'sell':
            prices = snapshot.bid_prices[:max_depth_levels]
            sizes = snapshot.bid_size_levels[:max_depth_levels]
        else:
            # Both sides: take minimum
            buy_cap = self.calculate_capacity_limit(fee_hurdle_bps, snapshot, 'buy', max_depth_levels)
            sell_cap = self.calculate_capacity_limit(fee_hurdle_bps, snapshot, 'sell', max_depth_levels)
            return {
                'buy_capacity_btc': buy_cap.get('capacity_btc', 0),
                'sell_capacity_btc': sell_cap.get('capacity_btc', 0),
                'capacity_btc': min(buy_cap.get('capacity_btc', 0), sell_cap.get('capacity_btc', 0)),
            }
        
        if len(prices) == 0:
            return {'capacity_btc': 0.0, 'error': 'no_depth'}
        
        best_price = prices[0]
        cumulative_sizes = []
        cumulative_cost = []
        total_qty = 0.0
        
        for price, size in zip(prices, sizes):
            total_qty += size
            cumulative_sizes.append(total_qty)
            cumulative_cost.append(total_qty * price)
        
        # Find max size where slippage <= fee hurdle
        capacity_btc = 0.0
        for i, (cum_size, cum_cost) in enumerate(zip(cumulative_sizes, cumulative_cost)):
            avg_price = cum_cost / cum_size if cum_size > 0 else best_price
            slippage = abs(avg_price - best_price)
            slippage_bps = (slippage / best_price) * 10000 if best_price > 0 else 0
            
            if slippage_bps <= fee_hurdle_bps:
                capacity_btc = cum_size / 100_000_000  # Convert satoshis to BTC
            else:
                break
        
        return {
            'capacity_btc': capacity_btc,
            'capacity_usd': capacity_btc * best_price,
            'levels_needed': i + 1 if 'i' in dir() else 0,
            'fee_hurdle_bps': fee_hurdle_bps,
        }
    
    def detect_phantom_liquidity(self, snapshot: LiquiditySnapshot,
                                  top_n: int = 3,
                                  depth_threshold: float = 0.7) -> Dict[str, float]:
        """
        Detect phantom liquidity (spoofing/layering) by comparing top levels to total depth.
        
        If top N levels represent >depth_threshold% of total visible depth,
        there's risk of phantom liquidity.
        
        Args:
            snapshot: Liquidity snapshot
            top_n: Number of top levels to analyze
            depth_threshold: Threshold for phantom liquidity flag
        
        Returns:
            Dict with phantom liquidity metrics
        """
        # Top N bid/ask depth
        top_bid_depth = np.sum(snapshot.bid_size_levels[:top_n] * snapshot.bid_prices[:top_n])
        top_ask_depth = np.sum(snapshot.ask_size_levels[:top_n] * snapshot.ask_prices[:top_n])
        
        total_bid_depth = snapshot.total_bid_depth
        total_ask_depth = snapshot.total_ask_depth
        
        bid_ratio = top_bid_depth / total_bid_depth if total_bid_depth > 0 else 0
        ask_ratio = top_ask_depth / total_ask_depth if total_ask_depth > 0 else 0
        
        # Flag if either side is heavily concentrated at top
        phantom_bid = bid_ratio > depth_threshold
        phantom_ask = ask_ratio > depth_threshold
        
        return {
            'top_bid_depth_usd': top_bid_depth,
            'top_ask_depth_usd': top_ask_depth,
            'total_bid_depth_usd': total_bid_depth,
            'total_ask_depth_usd': total_ask_depth,
            'bid_concentration': bid_ratio,
            'ask_concentration': ask_ratio,
            'phantom_liquidity_bid': phantom_bid,
            'phantom_liquidity_ask': phantom_ask,
            'phantom_liquidity_detected': phantom_bid or phantom_ask,
        }
    
    def compute_market_impact_coefficient(self, df_results: pd.DataFrame,
                                           qty_col: str = 'qty',
                                           slippage_col: str = 'slippage_bps') -> Dict[str, float]:
        """
        Compute market impact coefficient from execution results.
        
        Impact = a * (size / ADV)^b
        """
        # Filter valid data
        df = df_results[[qty_col, slippage_col]].dropna()
        df = df[df[slippage_col] > 0]
        
        if len(df) < 5:
            return {'a': np.nan, 'b': np.nan, 'r_squared': 0.0}
        
        return self.fit_slippage_curve(df[qty_col].values, df[slippage_col].values)
    
    def analyze_liquidity_resilience(self, df_results: pd.DataFrame,
                                      window: int = 100) -> pd.DataFrame:
        """
        Analyze how liquidity resilience changes with order size.
        
        Liquidity resilience = fill rate / order size
        """
        df = df_results.copy()
        
        # Group by order size
        grouped = df.groupby('qty').agg({
            'both_filled': ['count', 'sum', 'mean'],
            'perp_fill_qty': 'mean',
            'spot_fill_qty': 'mean',
        }).reset_index()
        
        grouped.columns = ['qty', 'n_signals', 'n_filled', 'fill_rate', 
                          'avg_perp_fill', 'avg_spot_fill']
        
        # Resilience metric: fill_rate / qty (higher = more resilient)
        grouped['liquidity_resilience'] = grouped['fill_rate'] / grouped['qty']
        
        # Slippage proxy from captured spread
        if 'captured_spread' in df.columns:
            grouped['avg_captured_spread'] = df.groupby('qty')['captured_spread'].mean().values
            grouped['avg_net_spread'] = df.groupby('qty')['net_spread'].mean().values
        
        return grouped


def generate_liquidity_report(df_results: pd.DataFrame,
                               fee_hurdle_bps: float = 4.0) -> Dict:
    """
    Generate comprehensive liquidity capacity report.
    
    Args:
        df_results: DataFrame from simulation results
        fee_hurdle_bps: Minimum spread needed to be profitable
    
    Returns:
        Dict with all capacity metrics
    """
    analyzer = LiquidityCapacityAnalyzer()
    
    report = {
        'n_total_signals': len(df_results),
        'n_filled': df_results['both_filled'].sum() if 'both_filled' in df_results.columns else 0,
        'fee_hurdle_bps': fee_hurdle_bps,
    }
    
    if report['n_total_signals'] == 0:
        return report
    
    report['fill_rate'] = report['n_filled'] / report['n_total_signals']
    
    # By order size
    by_size = []
    for qty in sorted(df_results['qty'].unique()):
        subset = df_results[df_results['qty'] == qty]
        n = len(subset)
        filled = subset['both_filled'].sum() if 'both_filled' in subset.columns else 0
        
        size_metrics = {
            'qty_btc': qty,
            'n_signals': n,
            'n_filled': filled,
            'fill_rate': filled / n if n > 0 else 0,
        }
        
        if 'net_spread' in subset.columns and filled > 0:
            filled_subset = subset[subset['both_filled']]
            size_metrics['avg_net_spread'] = filled_subset['net_spread'].mean()
            size_metrics['median_net_spread'] = filled_subset['net_spread'].median()
            size_metrics['profitable_rate'] = (filled_subset['net_spread'] > 0).mean()
        
        if 'time_to_fill_sec' in subset.columns:
            valid_ttf = subset['time_to_fill_sec'].dropna()
            if len(valid_ttf) > 0:
                size_metrics['avg_ttf_sec'] = valid_ttf.mean()
                size_metrics['median_ttf_sec'] = valid_ttf.median()
        
        by_size.append(size_metrics)
    
    report['by_order_size'] = by_size
    
    # Market impact coefficient
    if 'qty' in df_results.columns and 'slippage_bps' in df_results.columns:
        impact = analyzer.compute_market_impact_coefficient(df_results)
        report['market_impact'] = impact
    
    # Capacity limit estimate
    report['capacity_analysis'] = {
        'max_profitable_size_btc': np.nan,
        'note': 'Requires liquidity snapshot data for precise calculation',
    }
    
    return report