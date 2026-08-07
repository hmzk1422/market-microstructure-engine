"""
executor.py
===========
Main orchestrator for cross-exchange arbitrage execution and analysis.

Integrates:
- Fee hurdle calculations
- Decay rate analysis
- Liquidity capacity measurement
- ABIDES simulation results processing
- Research output generation

Usage:
    from executor import ArbitrationExecutor
    
    executor = ArbitrationExecutor()
    results = executor.run_full_analysis(simulation_results_path)
"""

import os
import json
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Union

import numpy as np
import pandas as pd

from .fee_hurdle import FeeHurdleCalculator, analyze_fee_hurdle_distribution
from .decay_analyzer import DecayAnalyzer, analyze_cross_exchange_decay
from .liquidity_capacity import (
    LiquidityCapacityAnalyzer,
    LiquiditySnapshot,
    generate_liquidity_report
)


class ArbitrationExecutor:
    """
    Production-grade executor for cross-exchange arbitrage research.
    
    Orchestrates the complete analysis pipeline:
    1. Load simulation results
    2. Calculate fee hurdles
    3. Analyze decay rates
    4. Measure liquidity capacity
    5. Generate comprehensive report
    
    Attributes:
        output_dir: Directory for all output files
        fee_calc: FeeHurdleCalculator instance
        decay_analyzer: DecayAnalyzer instance
        liquidity_analyzer: LiquidityCapacityAnalyzer instance
    """
    
    def __init__(self, output_dir: str = "simulator/research/output"):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        self.fee_calc = FeeHurdleCalculator()
        self.decay_analyzer = DecayAnalyzer()
        self.liquidity_analyzer = LiquidityCapacityAnalyzer()
        
        self.results: Dict = {}
        self.timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    
    def load_simulation_results(self, path: Union[str, Path]) -> pd.DataFrame:
        """Load ABIDES simulation results from parquet file."""
        path = Path(path)
        if not path.exists():
            raise FileNotFoundError(f"Results file not found: {path}")
        
        df = pd.read_parquet(path)
        print(f"[Executor] Loaded {len(df)} records from {path}")
        return df
    
    def analyze_fee_hurdles(self, df: pd.DataFrame,
                            trade_value_usd: float = 100000.0) -> Dict:
        """
        Analyze fee hurdles for the observed arbitrage opportunities.
        
        Args:
            df: Simulation results DataFrame
            trade_value_usd: Notional value for fee calculations
        
        Returns:
            Dict with fee hurdle analysis
        """
        print("[Executor] Analyzing fee hurdles...")
        
        # For perp-spot arb on same exchange, fee hurdle is minimal
        # Calculate for different scenarios
        
        scenarios = {
            'perp_spot_same_exchange': {
                'perp_venue': 'bybit',
                'spot_venue': 'bybit',
                'type': 'perp_spot'
            },
            'cex_to_cex': {
                'venue_a': 'binance',
                'venue_b': 'bybit',
                'type': 'cex_cex'
            },
            'cex_to_dex': {
                'cex': 'binance',
                'dex': 'uniswap_v3',
                'type': 'cex_dex'
            }
        }
        
        results = {}
        for scenario_name, params in scenarios.items():
            if params['type'] == 'perp_spot':
                hurdle = self.fee_calc.calculate_perp_spot_fee_hurdle(
                    perp_venue=params['perp_venue'],
                    spot_venue=params['spot_venue'],
                    trade_value_usd=trade_value_usd,
                    hold_hours=1.0,
                    funding_rate=0.10
                )
            elif params['type'] == 'cex_cex':
                hurdle = self.fee_calc.calculate_cex_fee_hurdle(
                    venue_a=params['venue_a'],
                    venue_b=params['venue_b'],
                    trade_value_usd=trade_value_usd
                )
            elif params['type'] == 'cex_dex':
                hurdle = self.fee_calc.calculate_cex_dex_fee_hurdle(
                    cex=params['cex'],
                    dex=params['dex'],
                    trade_value_usd=trade_value_usd,
                    gas_price_gwei=20.0
                )
            
            results[scenario_name] = hurdle
        
        # Add observed spreads from simulation
        if 'net_spread' in df.columns and df['net_spread'].notna().sum() > 0:
            filled_df = df[df['both_filled'] == True]
            if len(filled_df) > 0:
                results['observed_net_spread'] = {
                    'mean_bps': filled_df['net_spread'].mean(),
                    'median_bps': filled_df['net_spread'].median(),
                    'std_bps': filled_df['net_spread'].std(),
                    'min_bps': filled_df['net_spread'].min(),
                    'max_bps': filled_df['net_spread'].max(),
                }
        
        self.results['fee_hurdles'] = results
        return results
    
    def analyze_decay_rates(self, df: pd.DataFrame) -> Dict:
        """
        Analyze temporal decay of arbitrage opportunities.
        
        Args:
            df: Simulation results DataFrame
        
        Returns:
            Dict with decay analysis
        """
        print("[Executor] Analyzing decay rates...")
        
        analysis = {
            'n_signals': len(df),
            'n_filled': df['both_filled'].sum() if 'both_filled' in df.columns else 0,
        }
        
        if analysis['n_signals'] == 0:
            return analysis
        
        # Time-to-fill distribution
        if 'time_to_fill_sec' in df.columns:
            ttf_metrics = self.decay_analyzer.compute_decay_metrics(df)
            analysis['ttf_metrics'] = ttf_metrics
        
        # Decay by spread size
        if 'gross_spread_bps' in df.columns or 'net_spread' in df.columns:
            spread_col = 'gross_spread_bps' if 'gross_spread_bps' in df.columns else 'net_spread'
            quantile_analysis = self.decay_analyzer.analyze_decay_by_quantile(
                df, spread_col=spread_col
            )
            if len(quantile_analysis) > 0:
                analysis['spread_quantiles'] = quantile_analysis.to_dict('records')
        
        # Fill rate by order size
        if 'qty' in df.columns and 'both_filled' in df.columns:
            fill_by_size = df.groupby('qty').agg({
                'both_filled': ['count', 'sum', 'mean']
            }).reset_index()
            fill_by_size.columns = ['qty_btc', 'n_signals', 'n_filled', 'fill_rate']
            analysis['fill_by_size'] = fill_by_size.to_dict('records')
        
        self.results['decay_analysis'] = analysis
        return analysis
    
    def analyze_liquidity_capacity(self, df: pd.DataFrame,
                                    fee_hurdle_bps: float = 4.0) -> Dict:
        """
        Analyze liquidity capacity and capital limits.
        
        Args:
            df: Simulation results DataFrame
            fee_hurdle_bps: Minimum spread for profitability
        
        Returns:
            Dict with liquidity capacity analysis
        """
        print("[Executor] Analyzing liquidity capacity...")
        
        report = generate_liquidity_report(df, fee_hurdle_bps=fee_hurdle_bps)
        
        # Market impact curve
        if 'qty' in df.columns and 'net_spread' in df.columns:
            filled_df = df[df['both_filled'] == True]
            if len(filled_df) > 2:
                # Estimate slippage from spread compression
                impact = self.liquidity_analyzer.compute_market_impact_coefficient(
                    filled_df, qty_col='qty', slippage_col='net_spread'
                )
                report['market_impact_coefficient'] = impact
        
        self.results['liquidity_capacity'] = report
        return report
    
    def run_full_analysis(self, results_path: Union[str, Path],
                          trade_value_usd: float = 100000.0,
                          fee_hurdle_bps: float = 4.0) -> Dict:
        """
        Run complete analysis pipeline.
        
        Args:
            results_path: Path to simulation results parquet file
            trade_value_usd: Notional value for calculations
            fee_hurdle_bps: Minimum spread for profitability
        
        Returns:
            Complete analysis results dictionary
        """
        print("=" * 70)
        print("ARBITRATION EXECUTOR - FULL ANALYSIS")
        print("=" * 70)
        
        # Load results
        df = self.load_simulation_results(results_path)
        
        # Run all analyses
        fee_results = self.analyze_fee_hurdles(df, trade_value_usd)
        decay_results = self.analyze_decay_rates(df)
        liquidity_results = self.analyze_liquidity_capacity(df, fee_hurdle_bps)
        
        # Compile final report
        final_report = {
            'timestamp': self.timestamp,
            'simulation_file': str(results_path),
            'n_records': len(df),
            'fee_hurdle_analysis': fee_results,
            'decay_analysis': decay_results,
            'liquidity_capacity': liquidity_results,
            'research_questions': self._answer_research_questions(df, fee_results, decay_results, liquidity_results)
        }
        
        # Save report
        self._save_report(final_report)
        
        print("\n" + "=" * 70)
        print("ANALYSIS COMPLETE")
        print("=" * 70)
        print(f"Report saved to: {self.output_dir}/executor_report_{self.timestamp}.json")
        
        return final_report
    
    def _answer_research_questions(self, df: pd.DataFrame,
                                    fee_results: Dict,
                                    decay_results: Dict,
                                    liquidity_results: Dict) -> Dict:
        """Answer the three research questions."""
        answers = {}
        
        # RQ1: Decay rate and half-life
        if 'ttf_metrics' in decay_results:
            ttf = decay_results['ttf_metrics']
            answers['RQ1'] = {
                'question': 'What is the empirical decay rate and half-life of cross-exchange price discrepancies?',
                'findings': {
                    'time_to_fill_median_ms': ttf.get('ttf_median_ms', np.nan),
                    'time_to_fill_mean_ms': ttf.get('ttf_mean_ms', np.nan),
                    'time_to_fill_p95_ms': ttf.get('ttf_p95_ms', np.nan),
                    'note': 'Half-life estimated from time-to-fill distribution'
                }
            }
        
        # RQ2: Fee hurdle thresholds
        answers['RQ2'] = {
            'question': 'How do dynamic fee hurdles determine optimal net-of-fee entry thresholds?',
            'findings': {
                'perp_spot_same_exchange_bps': fee_results.get('perp_spot_same_exchange', {}).get('total_hurdle_bps', np.nan),
                'cex_cex_bps': fee_results.get('cex_to_cex', {}).get('total_hurdle_bps', np.nan),
                'cex_dex_bps': fee_results.get('cex_to_dex', {}).get('total_hurdle_bps', np.nan),
                'observed_net_spread': fee_results.get('observed_net_spread', {}),
            }
        }
        
        # RQ3: Capital capacity limits
        if 'by_order_size' in liquidity_results:
            by_size = liquidity_results['by_order_size']
            max_profitable = 0.0
            profitable_sizes = []
            
            for size_data in by_size:
                if size_data.get('profitable_rate', 0) > 0.5:
                    profitable_sizes.append(size_data['qty_btc'])
            
            if profitable_sizes:
                max_profitable = max(profitable_sizes)
            
            answers['RQ3'] = {
                'question': 'What are the effective capital capacity limits?',
                'findings': {
                    'max_profitable_size_btc': max_profitable,
                    'profitable_sizes': profitable_sizes,
                    'capacity_curve': by_size,
                    'note': 'Based on 50% profitability threshold'
                }
            }
        
        return answers
    
    def _save_report(self, report: Dict):
        """Save report to JSON file."""
        report_path = self.output_dir / f"executor_report_{self.timestamp}.json"
        
        # Convert numpy types to Python types for JSON serialization
        def convert_types(obj):
            if isinstance(obj, dict):
                return {k: convert_types(v) for k, v in obj.items()}
            elif isinstance(obj, list):
                return [convert_types(item) for item in obj]
            elif isinstance(obj, np.integer):
                return int(obj)
            elif isinstance(obj, np.floating):
                return float(obj)
            elif isinstance(obj, np.ndarray):
                return obj.tolist()
            elif isinstance(obj, np.bool_):
                return bool(obj)
            return obj
        
        report = convert_types(report)
        
        with open(report_path, 'w') as f:
            json.dump(report, f, indent=2, default=str)
        
        print(f"[Executor] Report saved: {report_path}")


def run_executor_on_latest_results(results_dir: str = "simulator/DeepMarket/ABIDES/research/data") -> Optional[Dict]:
    """
    Convenience function to run executor on latest simulation results.
    
    Args:
        results_dir: Directory containing simulation results
    
    Returns:
        Analysis report dict
    """
    results_path = Path(results_dir)
    if not results_path.exists():
        print(f"[Executor] Results directory not found: {results_path}")
        return None
    
    # Find the combined results file
    combined = results_path / "limit_order_all_qtys.parquet"
    if not combined.exists():
        # Find latest individual result
        parquet_files = sorted(results_path.glob("limit_order_qty_*.parquet"))
        if not parquet_files:
            print("[Executor] No result files found")
            return None
        results_file = parquet_files[-1]
    else:
        results_file = combined
    
    executor = ArbitrationExecutor()
    return executor.run_full_analysis(results_file)


if __name__ == "__main__":
    import sys
    
    if len(sys.argv) > 1:
        results_path = sys.argv[1]
    else:
        # Default to latest results
        results_path = "simulator/DeepMarket/ABIDES/research/data/limit_order_all_qtys.parquet"
    
    executor = ArbitrationExecutor()
    report = executor.run_full_analysis(results_path)
    
    # Print summary
    print("\n" + "=" * 70)
    print("EXECUTIVE SUMMARY")
    print("=" * 70)
    
    if 'research_questions' in report:
        for rq, data in report['research_questions'].items():
            print(f"\n{rq}: {data['question']}")
            for key, value in data['findings'].items():
                print(f"  {key}: {value}")
