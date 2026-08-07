"""
toxicity_filters.py
===================
Pre-trade toxicity filters to avoid adverse selection.

Implements:
1. VPIN (Volume-Synchronized Probability of Informed Trading)
2. OFI (Order Flow Imbalance)
3. IVR (Institutional Volume Ratio)
"""

import numpy as np
import pandas as pd
from typing import Dict, Tuple


class ToxicityFilters:
    """Multi-metric toxicity detection for limit order placement."""
    
    def __init__(self, 
                 vpin_window=50,
                 ofi_window=10,
                 retail_threshold=1.0,
                 institutional_threshold=10.0,
                 toxicity_threshold=0.7):
        self.vpin_window = vpin_window
        self.ofi_window = ofi_window
        self.retail_threshold = retail_threshold
        self.institutional_threshold = institutional_threshold
        self.toxicity_threshold = toxicity_threshold


def calculate_cvar_zscores(df: pd.DataFrame, 
                           window: int = 1000,
                           confidence: float = 0.05,
                           min_vol_usd: float = 1.0) -> pd.Series:
    """
    Calculate z-scores using CVaR (Conditional Value at Risk).
    
    More robust than mean/std for non-normal crypto returns.
    Uses quantile-based scaling instead of assuming normality.
    """
    df = df.copy()
    
    # Calculate mid-spread
    if 'mid_spread' not in df.columns:
        df['mid_spread'] = (df['perp_bid'] + df['perp_ask']) / 2 - \
                            (df['spot_bid'] + df['spot_ask']) / 2
    
    # Rolling median (robust location)
    rolling_median = df['mid_spread'].rolling(window, min_periods=100).median()
    
    # Rolling CVaR (Expected Shortfall beyond VaR)
    def calculate_rolling_cvar(x):
        if len(x) < 20:
            return abs(x.median() * 0.1)
        var = np.percentile(x, confidence * 100)
        cvar = x[x <= var].mean()
        return abs(cvar) if not np.isnan(cvar) else abs(x.median() * 0.1)
    
    rolling_cvar = df['mid_spread'].rolling(window, min_periods=100).apply(
        calculate_rolling_cvar, raw=True
    )
    
    # CVaR-based z-score (no normality assumption)
    cvar_z = (df['mid_spread'] - rolling_median) / (rolling_cvar + 1e-9)
    cvar_z = cvar_z.clip(-10, 10)
    
    return cvar_z


def calculate_institutional_volume_ratio(df: pd.DataFrame,
                                         window: int = 100,
                                         retail_threshold: float = 1.0,
                                         institutional_threshold: float = 10.0) -> pd.Series:
    """
    Calculate Institutional Volume Ratio (IVR).
    
    IVR measures the proportion of volume from "institutional-sized" orders.
    High IVR = market dominated by informed traders = toxic flow.
    
    Classification:
    - Retail: < 1 BTC
    - Mixed: 1-10 BTC
    - Institutional: > 10 BTC
    
    Args:
        df: DataFrame with trade data (must have 'size' column)
        window: Rolling window for ratio calculation
        retail_threshold: Orders below this size (BTC) are "retail"
        institutional_threshold: Orders above this size (BTC) are "institutional"
        
    Returns:
        Series of IVR values (0-1, higher = more institutional = more toxic)
    """
    df = df.copy()
    
    # Ensure size column is in BTC
    if 'size_btc' not in df.columns and 'size' in df.columns:
        df['size_btc'] = df['size']
    
    # Classify trades
    df['is_institutional'] = (df['size_btc'] >= institutional_threshold).astype(float)
    df['is_retail'] = (df['size_btc'] <= retail_threshold).astype(float)
    
    # Calculate rolling ratios
    inst_ratio = df['is_institutional'].rolling(window, min_periods=20).mean()
    retail_ratio = df['is_retail'].rolling(window, min_periods=20).mean()
    
    # Institutional Volume Ratio
    ivr = inst_ratio / (inst_ratio + retail_ratio + 1e-9)
    ivr = ivr.fillna(0.5)
    
    return ivr