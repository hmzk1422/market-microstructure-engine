#!/usr/bin/env python3
"""
cli.py
======
Command-line interface for the arbitration executor.

Usage:
    python -m executor.cli analyze <results_path> [options]
    python -m executor.cli batch <results_dir> [options]
    python -m executor.cli report <results_path> [options]
    
Examples:
    # Analyze single results file
    python -m executor.cli analyze research/data/limit_order_qty_0.1.parquet
    
    # Analyze with custom parameters
    python -m executor.cli analyze results.parquet --trade-value 50000 --fee-hurdle 5.0
    
    # Batch analyze all results in directory
    python -m executor.cli batch research/data/
    
    # Generate formatted report
    python -m executor.cli report results.parquet --output report.md
"""

import argparse
import sys
from pathlib import Path
from datetime import datetime
import json

from .executor import ArbitrationExecutor
from .fee_hurdle import FeeHurdleCalculator, analyze_fee_hurdle_distribution
from .decay_analyzer import DecayAnalyzer
from .liquidity_capacity import generate_liquidity_report


def format_number(value, decimals=2):
    """Format number for display."""
    if value is None or (isinstance(value, float) and (value != value)):  # NaN check
        return "N/A"
    if isinstance(value, float):
        if abs(value) >= 1e9:
            return f"{value:.2e}"
        return f"{value:.{decimals}f}"
    return str(value)


def print_executive_summary(report: dict):
    """Print formatted executive summary to console."""
    print("\n" + "=" * 70)
    print("EXECUTIVE SUMMARY")
    print("=" * 70)
    
    # Basic info
    print(f"\nTimestamp: {report.get('timestamp', 'N/A')}")
    print(f"Records analyzed: {report.get('n_records', 0)}")
    
    # Fee hurdles
    if 'fee_hurdle_analysis' in report:
        fee = report['fee_hurdle_analysis']
        print("\n" + "-" * 70)
        print("FEE HURDLE ANALYSIS")
        print("-" * 70)
        
        if 'perp_spot_same_exchange' in fee:
            ps = fee['perp_spot_same_exchange']
            print(f"\nPerp-Spot (Same Exchange):")
            print(f"  Total hurdle: {format_number(ps.get('total_hurdle_bps'))} bps")
            print(f"  Funding cost: {format_number(ps.get('funding_cost'))} USD")
        
        if 'cex_to_cex' in fee:
            cex = fee['cex_to_cex']
            print(f"\nCEX-to-CEX:")
            print(f"  Total hurdle: {format_number(cex.get('total_hurdle_bps'))} bps")
        
        if 'cex_to_dex' in fee:
            dex = fee['cex_to_dex']
            print(f"\nCEX-to-DEX:")
            print(f"  Total hurdle: {format_number(dex.get('total_hurdle_bps'))} bps")
            print(f"  Gas cost: {format_number(dex.get('gas_cost'))} USD")
        
        if 'observed_net_spread' in fee:
            obs = fee['observed_net_spread']
            print(f"\nObserved Net Spread (Filled Trades):")
            print(f"  Mean: {format_number(obs.get('mean_bps'))} bps")
            print(f"  Median: {format_number(obs.get('median_bps'))} bps")
            print(f"  Std: {format_number(obs.get('std_bps'))} bps")
    
    # Decay analysis
    if 'decay_analysis' in report:
        decay = report['decay_analysis']
        print("\n" + "-" * 70)
        print("DECAY ANALYSIS")
        print("-" * 70)
        print(f"\nTotal signals: {decay.get('n_signals', 0)}")
        print(f"Filled: {decay.get('n_filled', 0)}")
        
        if 'ttf_metrics' in decay:
            ttf = decay['ttf_metrics']
            print(f"\nTime-to-Fill (filled trades):")
            print(f"  Median: {format_number(ttf.get('ttf_median_ms'))} ms")
            print(f"  Mean: {format_number(ttf.get('ttf_mean_ms'))} ms")
            print(f"  P95: {format_number(ttf.get('ttf_p95_ms'))} ms")
            print(f"  Min: {format_number(ttf.get('ttf_min_ms'))} ms")
            print(f"  Max: {format_number(ttf.get('ttf_max_ms'))} ms")
    
    # Liquidity capacity
    if 'liquidity_capacity' in report:
        liq = report['liquidity_capacity']
        print("\n" + "-" * 70)
        print("LIQUIDITY CAPACITY")
        print("-" * 70)
        print(f"\nOverall fill rate: {format_number(liq.get('fill_rate', 0) * 100)}%")
        
        if 'by_order_size' in liq:
            print(f"\n{'Qty (BTC)':<12} {'Signals':<10} {'Filled':<10} {'Fill Rate':<12} {'Avg Net (bps)':<15}")
            print("-" * 70)
            for size_data in liq['by_order_size']:
                qty = size_data.get('qty_btc', 0)
                signals = size_data.get('n_signals', 0)
                filled = size_data.get('n_filled', 0)
                fill_rate = size_data.get('fill_rate', 0) * 100
                avg_net = size_data.get('avg_net_spread')
                print(f"{qty:<12.2f} {signals:<10} {filled:<10} {fill_rate:<12.2f} {format_number(avg_net, 2):<15}")
    
    # Research questions
    if 'research_questions' in report:
        print("\n" + "=" * 70)
        print("RESEARCH QUESTIONS")
        print("=" * 70)
        for rq_id, rq_data in report['research_questions'].items():
            print(f"\n{rq_id}: {rq_data['question']}")
            for key, value in rq_data['findings'].items():
                if isinstance(value, list) and len(value) > 5:
                    print(f"  {key}: [{len(value)} items]")
                else:
                    print(f"  {key}: {value}")
    
    print("\n" + "=" * 70)


def generate_markdown_report(report: dict, output_path: Path):
    """Generate formatted Markdown report."""
    lines = []
    lines.append("# Cross-Exchange Arbitrage Research Report")
    lines.append(f"\n**Generated:** {report.get('timestamp', datetime.now().isoformat())}")
    lines.append(f"\n**Data Source:** {report.get('simulation_file', 'N/A')}")
    lines.append(f"\n**Records Analyzed:** {report.get('n_records', 0)}")
    
    # Research Questions
    lines.append("\n## Research Questions")
    lines.append("\n### RQ1: Decay Rate and Half-Life")
    if 'research_questions' in report and 'RQ1' in report['research_questions']:
        rq1 = report['research_questions']['RQ1']
        lines.append(f"\n**Question:** {rq1['question']}")
        lines.append("\n**Findings:**")
        for key, value in rq1['findings'].items():
            if key != 'note':
                lines.append(f"- {key}: {value}")
        if 'note' in rq1['findings']:
            lines.append(f"\n*{rq1['findings']['note']}*")
    
    lines.append("\n### RQ2: Fee Hurdle Thresholds")
    if 'research_questions' in report and 'RQ2' in report['research_questions']:
        rq2 = report['research_questions']['RQ2']
        lines.append(f"\n**Question:** {rq2['question']}")
        lines.append("\n**Findings:**")
        for key, value in rq2['findings'].items():
            if key != 'observed_net_spread' and key != 'note':
                lines.append(f"- {key}: {format_number(value)} bps")
            elif key == 'observed_net_spread':
                lines.append(f"\n**Observed Net Spread (Filled Trades):**")
                for k, v in value.items():
                    lines.append(f"- {k}: {format_number(v)} bps")
    
    lines.append("\n### RQ3: Capital Capacity Limits")
    if 'research_questions' in report and 'RQ3' in report['research_questions']:
        rq3 = report['research_questions']['RQ3']
        lines.append(f"\n**Question:** {rq3['question']}")
        lines.append("\n**Findings:**")
        for key, value in rq3['findings'].items():
            if key == 'capacity_curve':
                lines.append("\n**Capacity Curve by Order Size:**")
                lines.append("\n| Qty (BTC) | Signals | Filled | Fill Rate | Avg Net Spread (bps) |")
                lines.append("|-----------|---------|--------|-----------|---------------------|")
                for size_data in value:
                    qty = size_data.get('qty_btc', 0)
                    signals = size_data.get('n_signals', 0)
                    filled = size_data.get('n_filled', 0)
                    fill_rate = size_data.get('fill_rate', 0)
                    avg_net = format_number(size_data.get('avg_net_spread'))
                    lines.append(f"| {qty:.2f} | {signals} | {filled} | {fill_rate:.2%} | {avg_net} |")
            elif key != 'note':
                lines.append(f"- {key}: {value}")
        if 'note' in rq3['findings']:
            lines.append(f"\n*{rq3['findings']['note']}*")
    
    # Fee Hurdle Details
    if 'fee_hurdle_analysis' in report:
        lines.append("\n## Fee Hurdle Analysis")
        fee = report['fee_hurdle_analysis']
        
        lines.append("\n### Scenario Comparison")
        lines.append("\n| Scenario | Fee Hurdle (bps) |")
        lines.append("|----------|-----------------|")
        for scenario in ['perp_spot_same_exchange', 'cex_to_cex', 'cex_to_dex']:
            if scenario in fee:
                hurdle = format_number(fee[scenario].get('total_hurdle_bps'))
                lines.append(f"| {scenario} | {hurdle} |")
    
    # Liquidity Capacity Details
    if 'liquidity_capacity' in report:
        lines.append("\n## Liquidity Capacity Analysis")
        liq = report['liquidity_capacity']
        lines.append(f"\n**Overall Fill Rate:** {format_number(liq.get('fill_rate', 0) * 100)}%")
        
        if 'market_impact_coefficient' in liq:
            impact = liq['market_impact_coefficient']
            lines.append(f"\n**Market Impact Coefficient:**")
            lines.append(f"- a = {format_number(impact.get('a'))}")
            lines.append(f"- b = {format_number(impact.get('b'))}")
            lines.append(f"- R² = {format_number(impact.get('r_squared'), 4)}")
    
    # Methodology
    lines.append("\n## Methodology")
    lines.append("""
### Fee Hurdle Calculation
Fee hurdles account for:
- **CEX Trading Fees:** Maker/taker fees (tiered by volume)
- **DEX Gas Costs:** L1/L2 transaction costs
- **Perpetual Funding:** Funding rate payments over holding period
- **Withdrawal Fees:** Cross-venue transfer costs

### Decay Rate Analysis
Temporal decay measured via:
- Time-to-fill distribution
- Exponential/Weibull decay model fitting
- Quantile analysis by spread size

### Liquidity Capacity
Capital limits determined by:
- Order book depth analysis
- Slippage curve fitting (power-law model)
- Phantom liquidity detection
""")
    
    # Save report
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))
    
    print(f"[CLI] Markdown report saved: {output_path}")


def cmd_analyze(args):
    """Execute analyze command."""
    results_path = Path(args.results)
    if not results_path.exists():
        print(f"Error: Results file not found: {results_path}")
        sys.exit(1)
    
    executor = ArbitrationExecutor(output_dir=args.output_dir)
    report = executor.run_full_analysis(
        results_path=results_path,
        trade_value_usd=args.trade_value,
        fee_hurdle_bps=args.fee_hurdle
    )
    
    print_executive_summary(report)
    
    # Save JSON report
    if args.json:
        json_path = Path(args.json)
        with open(json_path, 'w') as f:
            json.dump(report, f, indent=2, default=str)
        print(f"\n[CLI] JSON report saved: {json_path}")


def cmd_batch(args):
    """Execute batch command."""
    results_dir = Path(args.results_dir)
    if not results_dir.exists():
        print(f"Error: Directory not found: {results_dir}")
        sys.exit(1)
    
    # Find all parquet files
    parquet_files = sorted(results_dir.glob("*.parquet"))
    if not parquet_files:
        print(f"No parquet files found in {results_dir}")
        sys.exit(1)
    
    print(f"Found {len(parquet_files)} result files")
    
    executor = ArbitrationExecutor(output_dir=args.output_dir)
    all_reports = []
    
    for pf in parquet_files:
        print(f"\nAnalyzing: {pf.name}")
        try:
            report = executor.run_full_analysis(
                results_path=pf,
                trade_value_usd=args.trade_value,
                fee_hurdle_bps=args.fee_hurdle
            )
            all_reports.append(report)
        except Exception as e:
            print(f"  Error: {e}")
    
    # Combine results
    if all_reports:
        combined_path = Path(args.output_dir) / "batch_report.json"
        with open(combined_path, 'w') as f:
            json.dump(all_reports, f, indent=2, default=str)
        print(f"\n[CLI] Combined batch report saved: {combined_path}")


def cmd_report(args):
    """Execute report command."""
    results_path = Path(args.results)
    if not results_path.exists():
        print(f"Error: Results file not found: {results_path}")
        sys.exit(1)
    
    executor = ArbitrationExecutor(output_dir=args.output_dir)
    report = executor.run_full_analysis(
        results_path=results_path,
        trade_value_usd=args.trade_value,
        fee_hurdle_bps=args.fee_hurdle
    )
    
    # Generate markdown report
    md_path = Path(args.output) or Path(args.output_dir) / "report.md"
    generate_markdown_report(report, md_path)
    
    print_executive_summary(report)


def main():
    """Main CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Cross-Exchange Arbitrage Research Executor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Analyze single file
  python -m executor.cli analyze results.parquet
  
  # Generate markdown report
  python -m executor.cli report results.parquet --output report.md
  
  # Batch analysis
  python -m executor.cli batch data/
        """
    )
    
    subparsers = parser.add_subparsers(dest='command', help='Available commands')
    
    # Analyze command
    analyze_parser = subparsers.add_parser('analyze', help='Analyze simulation results')
    analyze_parser.add_argument('results', help='Path to results parquet file')
    analyze_parser.add_argument('--trade-value', type=float, default=100000.0,
                               help='Notional trade value in USD (default: 100000)')
    analyze_parser.add_argument('--fee-hurdle', type=float, default=4.0,
                               help='Fee hurdle threshold in bps (default: 4.0)')
    analyze_parser.add_argument('--output-dir', default='simulator/research/output',
                               help='Output directory (default: simulator/research/output)')
    analyze_parser.add_argument('--json', help='Path to save JSON report')
    
    # Batch command
    batch_parser = subparsers.add_parser('batch', help='Batch analyze multiple files')
    batch_parser.add_argument('results_dir', help='Directory containing result files')
    batch_parser.add_argument('--trade-value', type=float, default=100000.0)
    batch_parser.add_argument('--fee-hurdle', type=float, default=4.0)
    batch_parser.add_argument('--output-dir', default='simulator/research/output')
    
    # Report command
    report_parser = subparsers.add_parser('report', help='Generate formatted report')
    report_parser.add_argument('results', help='Path to results parquet file')
    report_parser.add_argument('--output', help='Output path for markdown report')
    report_parser.add_argument('--trade-value', type=float, default=100000.0)
    report_parser.add_argument('--fee-hurdle', type=float, default=4.0)
    report_parser.add_argument('--output-dir', default='simulator/research/output')
    
    args = parser.parse_args()
    
    if args.command is None:
        parser.print_help()
        sys.exit(1)
    
    if args.command == 'analyze':
        cmd_analyze(args)
    elif args.command == 'batch':
        cmd_batch(args)
    elif args.command == 'report':
        cmd_report(args)


if __name__ == "__main__":
    main()