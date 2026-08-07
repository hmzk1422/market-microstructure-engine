"""
Production-grade executor infrastructure for cross-exchange arbitrage research.

Modules:
    fee_hurdle.py           - Dynamic fee-hurdle threshold calculations
    decay_analyzer.py       - Temporal decay rate and half-life measurement
    empirical_decay.py      - Economic TTR decay analysis with Kaplan-Meier
    liquidity_capacity.py   - Capital capacity and slippage analysis
    executor.py             - Main orchestrator
"""

from .executor import ArbitrationExecutor
from .fee_hurdle import FeeHurdleCalculator, analyze_fee_hurdle_distribution
from .decay_analyzer import DecayAnalyzer, analyze_cross_exchange_decay
from .empirical_decay import run_economic_backtest, kaplan_meier, generate_economic_report
from .liquidity_capacity import (
    LiquidityCapacityAnalyzer,
    LiquiditySnapshot,
    generate_liquidity_report
)

__all__ = [
    'ArbitrationExecutor',
    'FeeHurdleCalculator',
    'analyze_fee_hurdle_distribution',
    'DecayAnalyzer',
    'analyze_cross_exchange_decay',
    'run_economic_backtest',
    'kaplan_meier',
    'generate_economic_report',
    'LiquidityCapacityAnalyzer',
    'LiquiditySnapshot',
    'generate_liquidity_report',
]
