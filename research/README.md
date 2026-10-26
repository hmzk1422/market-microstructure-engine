# Quant research workspace

Use this directory for reproducible strategy research: notebooks, experiment
configuration, backtest summaries, and methodology notes. Keep production C++
modules under `hardware/` and exchange feed adapters under `exchange/` so
research experiments do not become runtime dependencies.

Do not commit raw exchange credentials or unlicensed market data. Record data
source, timestamp assumptions, fee/slippage assumptions, and random seeds with
each experiment.
