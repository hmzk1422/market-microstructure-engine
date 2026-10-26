# Benchmark and Evidence

## Historical WebSocket run

The values below were recorded before the current strategy, queue, and telemetry
changes. They are retained as historical context only; they are not a baseline
for the current code and have not been reproduced in this revision.

| Measurement | Historical value |
| --- | ---: |
| Source / configured symbols | WebSocket / 2 |
| Runtime / updates | 21.305 s / 1,520 |
| Throughput | 71.3 RTL updates/s |
| WebSocket callback to Verilator output, p50 / p99 / max | 60.719 μs / 10.550 ms / 84.252 ms |
| Verilator eval wall time, p50 / p99 / max | 157 ns / 5.586 μs / 18.184 μs |
| Callback to strategy enqueue, p50 / p99 / max | 62.605 μs / 18.590 ms / 105.809 ms |
| Strategy queue wait, p50 / p99 / max | 28.844 μs / 161.705 μs / 42.747 ms |

These are software timings from a WebSocket callback through a Verilated model.
`eval()` wall time measures the CPU executing generated C++ for the RTL; it is
not FPGA timing. Percentiles from separate stages cannot be added to infer an
end-to-end percentile, and the previous report's arithmetic did that. The
previous report also listed FPGA resource numbers without traceable target-flow
evidence. Do not present those as verified hardware results.

## Current measurement contract

The runnable application reports callback-to-RTL, callback-to-paper-decision,
RTL evaluation, strategy queue wait, and a new RTL→C++→RTL acknowledgement
round-trip. The latter begins at C++ handoff and ends when the matching
transaction ID is acknowledged by the Verilated RTL. The optional
`--latency-every-message` mode prints each paired quote's round trip. A fixed
histogram keeps recording bounded; percentiles are approximate (1 μs buckets
below 1 ms, 1 ms buckets below 1 s). These are local software timings and do
not include WebSocket transport before callback entry or exchange order I/O.

The application also compares three *modeled* network routes: firms in
Singapore, Tokyo, and their geographic midpoint. Propagation uses a 1.35 route
stretch and fiber refractive index of 1.468. OU-style jitter is calibrated from
WebSocket interarrival variation and rare positive Pareto spikes add a heavy
tail. These assumptions produce an illustrative model, not measured exchange
server locations or actual route latency. Bybit's API server location is
described as Singapore in its own help material; the Binance Tokyo endpoint is
an explicit scenario assumption, not a verified matching-engine location.

The strategy worker reads fixed-size price/candidate records from a named OS
shared-memory ring, recomputes the fee-adjusted spread, and returns a decision
and quantity through a separate ring for an RTL acknowledgement. This is a
software DMA stand-in. TimescaleDB stores raw scaled quote prices in
`market_prices`, per-input RTL candidate and C++ strategy results in
`strategy_decisions`, one row per RTL acknowledgement in `latency_samples`, and
once-per-second resource/latency/trade/PnL snapshots in `runtime_metrics`.
Each latency row preserves monotonic `t_send_steady_ns` and
`t_receive_steady_ns` alongside their round-trip difference.
`exchange_event_age_ms`
is local receive time minus the exchange event timestamp; clock offset means
it is not a one-way network latency measurement. PnL is paper realized PnL and
uses the configured fees, slippage, and fixed position size. The default fee
and slippage values are zero for demonstration.

## Reproduce

Build and run the correctness suite:

```bash
cmake -S hardware -B hardware/build -DCMAKE_BUILD_TYPE=Release
cmake --build hardware/build --parallel 2
ctest --test-dir hardware/build --output-on-failure
```

The strategy unit test exercises warm-up, zero variance, rolling eviction, risk
gating, and 512 independent symbol windows. To exercise the complete Verilated
pipeline across 512 synthetic symbols, run from the repository root in WSL:

```bash
symbols=$(python3 -c 'print(",".join(f"ASSET{i:04d}" for i in range(512)))')
./hardware/build/market_data_engine --source synthetic --symbols "$symbols" \
  --duration-sec 3 --synthetic-tick-ms 10 --no-telemetry
```

The synthetic source creates deterministic wide-positive, narrow-positive, and
negative spread phases, so this run exercises warm-up, z-score acceptance,
paper entries/exits, bounded queue drops, and measured software latency without
exchange access or a database. The
printed update, candidate, trade, and drop counts should accompany any claimed
throughput result. The synthetic feed is a load fixture, not evidence of
exchange performance or profitability.

CI builds the Verilated executable and runs the RTL acknowledgement, strategy,
mapped-ring, and network-model tests on Ubuntu. No physical FPGA implementation,
Xilinx timing closure, hardware DMA path, kernel-bypass network stack, or live
order execution is measured by this benchmark.


```bash
python3 hardware/test/latency_resource_report.py \
  --binary hardware/build/market_data_engine \
  --duration-sec 3600 --symbols 64
```
Returns a summary report for the system's overall performance throughout a certain period of time.