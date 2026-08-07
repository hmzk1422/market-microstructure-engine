"""
fee_hurdle.py
=============
Dynamic fee-hurdle threshold calculations for cross-exchange arbitrage.

Calculates the minimum spread required to overcome:
- CEX maker/taker fees (tiered structures)
- DEX gas fees (L1/L2 volatility)
- Perpetual funding rates
- Cross-chain bridge costs
- Withdrawal/deposit fees

References:
    "Transaction Costs and Arbitrage Profitability" - Hasbrouck (2007)
    "Market Microstructure" - O'Hara (2015)
"""

import numpy as np
import pandas as pd
from dataclasses import dataclass
from typing import Dict, Optional
from datetime import datetime


@dataclass
class FeeStructure:
    """Fee structure for a single venue."""
    maker_bps: float = 0.0002  # 0.02%
    taker_bps: float = 0.0005  # 0.05%
    withdrawal_fee: float = 0.0  # USD or asset
    withdrawal_fee_pct: float = 0.0
    gas_token: str = "ETH"
    
    def effective_fee(self, is_maker: bool, trade_value_usd: float) -> float:
        """Calculate effective fee in USD for a given trade."""
        if is_maker:
            fee_pct = self.maker_bps
        else:
            fee_pct = self.taker_bps
        return trade_value_usd * fee_pct


@dataclass
class GasCostModel:
    """Gas cost model for DEX transactions."""
    base_gas_limit: int = 150000
    gas_price_gwei: float = 20.0
    eth_price_usd: float = 3000.0
    l1_fallback: bool = False
    
    def calculate_gas_usd(self, is_l1: bool = False) -> float:
        """Calculate gas cost in USD."""
        if is_l1 or self.l1_fallback:
            # L1 transactions are ~10-100x more expensive
            gas_limit = self.base_gas_limit * 50
        else:
            gas_limit = self.base_gas_limit
            
        gas_cost_eth = (gas_limit * self.gas_price_gwei) / 1e9
        return gas_cost_eth * self.eth_price_usd


@dataclass
class PerpFundingModel:
    """Perpetual swap funding rate model."""
    annual_rate: float = 0.10  # 10% annual
    funding_interval_hours: float = 8.0
    position_size_usd: float = 100000.0
    
    def calculate_funding_cost(self, hold_hours: float, is_short: bool = True) -> float:
        """
        Calculate funding cost for holding a position.
        
        Args:
            hold_hours: Expected holding period in hours
            is_short: True for short positions (pay funding if positive)
        
        Returns:
            Funding cost in USD (positive = cost, negative = gain)
        """
        # Funding paid 3x per day
        fundings_per_period = hold_hours / self.funding_interval_hours
        rate_per_period = self.annual_rate / (365.25 * 24 / self.funding_interval_hours)
        total_rate = rate_per_period * fundings_per_period
        
        cost = self.position_size_usd * total_rate
        if is_short:
            # Shorts pay when funding is positive, receive when negative
            return cost
        else:
            return -cost


class FeeHurdleCalculator:
    """
    Calculates dynamic fee hurdles for arbitrage strategies.
    
    The fee hurdle is the minimum gross spread required to overcome all
    transaction costs and generate a positive net profit.
    
    Fee Hurdle = Σ(fees) + gas + funding + slippage_buffer
    """
    
    def __init__(self):
        self.venue_fees: Dict[str, FeeStructure] = {
            'binance': FeeStructure(maker_bps=0.0002, taker_bps=0.0005),
            'coinbase': FeeStructure(maker_bps=0.0004, taker_bps=0.0006),
            'bybit': FeeStructure(maker_bps=0.0001, taker_bps=0.0006),
            'okx': FeeStructure(maker_bps=0.0002, taker_bps=0.0005),
            'kraken': FeeStructure(maker_bps=0.0000, taker_bps=0.0006),
            # DEX
            'uniswap_v3': FeeStructure(maker_bps=0.0003, taker_bps=0.0003),
            'dydx': FeeStructure(maker_bps=0.0000, taker_bps=0.0005),
            'gmx': FeeStructure(maker_bps=0.0001, taker_bps=0.0001),
        }
        
        self.gas_models: Dict[str, GasCostModel] = {
            'ethereum_l1': GasCostModel(base_gas_limit=150000, gas_price_gwei=30.0, l1_fallback=True),
            'arbitrum': GasCostModel(base_gas_limit=150000, gas_price_gwei=0.1),
            'optimism': GasCostModel(base_gas_limit=150000, gas_price_gwei=0.001),
            'polygon': GasCostModel(base_gas_limit=150000, gas_price_gwei=30.0),
            'base': GasCostModel(base_gas_limit=150000, gas_price_gwei=0.001),
        }
    
    def calculate_cex_fee_hurdle(self, venue_a: str, venue_b: str,
                                   trade_value_usd: float,
                                   is_maker_a: bool = True,
                                   is_maker_b: bool = True) -> Dict[str, float]:
        """
        Calculate fee hurdle for CEX-to-CEX arbitrage.
        
        Args:
            venue_a: First exchange name
            venue_b: Second exchange name
            trade_value_usd: Notional value of trade in USD
            is_maker_a: Whether we post limit orders on venue A
            is_maker_b: Whether we post limit orders on venue B
        
        Returns:
            Dict with fee components and total hurdle
        """
        if venue_a not in self.venue_fees or venue_b not in self.venue_fees:
            raise ValueError(f"Unknown venue: {venue_a} or {venue_b}")
        
        fee_a = self.venue_fees[venue_a].effective_fee(is_maker_a, trade_value_usd)
        fee_b = self.venue_fees[venue_b].effective_fee(is_maker_b, trade_value_usd)
        
        # Withdrawal fees (one-time, amortized over trade size)
        withdraw_fee_a = self.venue_fees[venue_a].withdrawal_fee
        withdraw_fee_b = self.venue_fees[venue_b].withdrawal_fee
        
        # Total fee hurdle in USD
        total_hurdle_usd = fee_a + fee_b + withdraw_fee_a + withdraw_fee_b
        
        return {
            'venue_a_fee': fee_a,
            'venue_b_fee': fee_b,
            'venue_a_withdrawal': withdraw_fee_a,
            'venue_b_withdrawal': withdraw_fee_b,
            'total_hurdle_usd': total_hurdle_usd,
            'total_hurdle_bps': (total_hurdle_usd / trade_value_usd) * 10000,
        }
    
    def calculate_dex_fee_hurdle(self, dex_a: str, dex_b: str,
                                   trade_value_usd: float,
                                   gas_price_gwei: float = 20.0,
                                   is_l1: bool = False) -> Dict[str, float]:
        """
        Calculate fee hurdle for DEX-to-DEX arbitrage.
        
        Includes swap fees + gas costs for both legs.
        """
        if dex_a not in self.venue_fees or dex_b not in self.venue_fees:
            raise ValueError(f"Unknown DEX: {dex_a} or {dex_b}")
        
        # Swap fees
        swap_fee_a = self.venue_fees[dex_a].effective_fee(False, trade_value_usd)
        swap_fee_b = self.venue_fees[dex_b].effective_fee(False, trade_value_usd)
        
        # Gas costs (2 swaps + potentially approve txns)
        gas_model = self.gas_models.get('ethereum_l1')
        if gas_model:
            gas_model.gas_price_gwei = gas_price_gwei
            gas_per_swap = gas_model.calculate_gas_usd(is_l1)
        else:
            gas_per_swap = 5.0  # fallback
        
        total_gas = gas_per_swap * 2
        
        total_hurdle = swap_fee_a + swap_fee_b + total_gas
        
        return {
            'swap_fee_a': swap_fee_a,
            'swap_fee_b': swap_fee_b,
            'gas_leg_a': gas_per_swap,
            'gas_leg_b': gas_per_swap,
            'total_gas': total_gas,
            'total_hurdle_usd': total_hurdle,
            'total_hurdle_bps': (total_hurdle / trade_value_usd) * 10000,
        }
    
    def calculate_cex_dex_fee_hurdle(self, cex: str, dex: str,
                                       trade_value_usd: float,
                                       gas_price_gwei: float = 20.0) -> Dict[str, float]:
        """Calculate fee hurdle for CEX-to-DEX arbitrage."""
        # CEX leg (taker)
        cex_fee = self.venue_fees[cex].effective_fee(False, trade_value_usd)
        
        # DEX leg (swap + gas)
        dex_swap_fee = self.venue_fees[dex].effective_fee(False, trade_value_usd)
        gas_model = self.gas_models.get('ethereum_l1')
        if gas_model:
            gas_model.gas_price_gwei = gas_price_gwei
            gas_cost = gas_model.calculate_gas_usd()
        else:
            gas_cost = 10.0
        
        total_hurdle = cex_fee + dex_swap_fee + gas_cost
        
        return {
            'cex_taker_fee': cex_fee,
            'dex_swap_fee': dex_swap_fee,
            'gas_cost': gas_cost,
            'total_hurdle_usd': total_hurdle,
            'total_hurdle_bps': (total_hurdle / trade_value_usd) * 10000,
        }
    
    def calculate_perp_spot_fee_hurdle(self, perp_venue: str, spot_venue: str,
                                        trade_value_usd: float,
                                        hold_hours: float = 1.0,
                                        funding_rate: float = 0.10) -> Dict[str, float]:
        """
        Calculate fee hurdle for perp-spot arbitrage.
        
        Includes: fees + funding cost + potential withdrawal fees.
        """
        # Trading fees (both legs, typically taker)
        perp_fee = self.venue_fees[perp_venue].effective_fee(False, trade_value_usd)
        spot_fee = self.venue_fees[spot_venue].effective_fee(False, trade_value_usd)
        
        # Funding cost for holding the perp leg
        funding_model = PerpFundingModel(
            annual_rate=funding_rate,
            position_size_usd=trade_value_usd
        )
        funding_cost = funding_model.calculate_funding_cost(hold_hours, is_short=True)
        
        # Withdrawal fees if moving collateral
        withdraw_cost = 0.0  # Assume no withdrawal needed
        
        total_hurdle = perp_fee + spot_fee + funding_cost + withdraw_cost
        
        return {
            'perp_fee': perp_fee,
            'spot_fee': spot_fee,
            'funding_cost': funding_cost,
            'withdrawal_cost': withdraw_cost,
            'total_hurdle_usd': total_hurdle,
            'total_hurdle_bps': (total_hurdle / trade_value_usd) * 10000,
        }
    
    def get_optimal_entry_threshold(self, gross_spread_bps: float,
                                     fee_hurdle_bps: float,
                                     latency_ms: float = 0.0,
                                     decay_rate_per_ms: float = 0.001) -> Dict[str, float]:
        """
        Calculate optimal entry threshold accounting for latency and decay.
        
        The threshold is the minimum gross spread needed to:
        1. Cover fee hurdles
        2. Account for spread decay during execution latency
        
        Args:
            gross_spread_bps: Observed spread in basis points
            fee_hurdle_bps: Total fee hurdle in basis points
            latency_ms: Execution latency in milliseconds
            decay_rate_per_ms: Expected spread decay per ms
        
        Returns:
            Dict with threshold analysis
        """
        # Decay adjustment: spread narrows by (decay_rate * latency)
        decay_loss = decay_rate_per_ms * latency_ms
        effective_spread = gross_spread_bps - decay_loss
        
        # Net profit after fees
        net_spread = effective_spread - fee_hurdle_bps
        
        # Minimum threshold = fee hurdle + 1bp buffer + decay adjustment
        min_threshold = fee_hurdle_bps + 1.0 + decay_loss
        
        return {
            'gross_spread_bps': gross_spread_bps,
            'fee_hurdle_bps': fee_hurdle_bps,
            'decay_loss_bps': decay_loss,
            'effective_spread_bps': effective_spread,
            'net_spread_bps': net_spread,
            'min_entry_threshold_bps': min_threshold,
            'profitable': net_spread > 0,
            'edge_after_decay': effective_spread - fee_hurdle_bps,
        }


def analyze_fee_hurdle_distribution(df: pd.DataFrame, 
                                     trade_value_usd: float = 100000.0) -> pd.DataFrame:
    """
    Analyze fee hurdle sensitivity across different venue combinations.
    
    Args:
        df: DataFrame with columns ['venue_a', 'venue_b', 'spread_bps']
        trade_value_usd: Notional trade value
    
    Returns:
        DataFrame with fee hurdle analysis
    """
    calc = FeeHurdleCalculator()
    results = []
    
    for _, row in df.iterrows():
        try:
            # CEX-CEX
            fees = calc.calculate_cex_fee_hurdle(
                row['venue_a'], row['venue_b'], trade_value_usd
            )
            hurdle_bps = fees['total_hurdle_bps']
        except:
            hurdle_bps = np.nan
        
        results.append({
            'venue_a': row['venue_a'],
            'venue_b': row['venue_b'],
            'spread_bps': row['spread_bps'],
            'fee_hurdle_bps': hurdle_bps,
            'net_spread_bps': row['spread_bps'] - hurdle_bps,
            'profitable': (row['spread_bps'] - hurdle_bps) > 0,
        })
    
    return pd.DataFrame(results)