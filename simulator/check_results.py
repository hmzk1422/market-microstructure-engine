import json

with open('research/output/executor_report_20260806_014209.json') as f:
    report = json.load(f)

print('=== RESEARCH QUESTIONS ===')
for rq, data in report['research_questions'].items():
    print(f"\n{rq}: {data['question']}")
    print("Findings:")
    for key, value in data['findings'].items():
        if isinstance(value, (int, float)):
            print(f"  {key}: {value:.4f}")
        else:
            print(f"  {key}: {value}")

print('\n=== FEE HURDLE RESULTS ===')
fee = report['fee_hurdle_analysis']
print(f"Perp-Spot Same Exchange: {fee.get('perp_spot_same_exchange', {}).get('total_hurdle_bps', 'N/A')} bps")
print(f"CEX-to-CEX: {fee.get('cex_to_cex', {}).get('total_hurdle_bps', 'N/A')} bps")
print(f"CEX-to-DEX: {fee.get('cex_to_dex', {}).get('total_hurdle_bps', 'N/A')} bps")

print('\n=== DECAY ANALYSIS ===')
decay = report['decay_analysis']
print(f"Total signals: {decay.get('n_signals', 0)}")
print(f"Total filled: {decay.get('n_filled', 0)}")
if 'ttf_metrics' in decay:
    print(f"Median TTF: {decay['ttf_metrics'].get('ttf_median_ms', 'N/A')} ms")

print('\n=== LIQUIDITY CAPACITY ===')
liq = report['liquidity_capacity']
if 'by_order_size' in liq:
    print(f"Analyzed {len(liq['by_order_size'])} order sizes")
    profitable = [s for s in liq['by_order_size'] if s.get('profitable_rate', 0) > 0.5]
    print(f"Profitable sizes (>50% win rate): {[s['qty_btc'] for s in profitable]}")