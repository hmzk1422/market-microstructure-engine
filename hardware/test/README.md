# Hardware tests and performance report

The C++ unit tests check individual mechanisms: z-score math, bounded SPSC and
mmap ring correctness, route-model behavior, and RTL decisions. Those mechanisms
are runtime implementation; their unit tests do not claim a full-system speedup.

Build and run correctness tests:

```sh
cmake -S hardware -B hardware/build -DCMAKE_BUILD_TYPE=Release
cmake --build hardware/build --target market_data_engine --parallel 2
ctest --test-dir hardware/build --output-on-failure
```

The engine target also builds the registered test executables, so the target-only
build followed by CTest works from a clean build directory.

Run a one-hour, 64-symbol synthetic performance test:

```sh
python3 hardware/test/latency_resource_report.py \
  --binary hardware/build/market_data_engine \
  --duration-sec 3600 --symbols 64
```

This test disables TimescaleDB and per-message console logging. The engine uses
bounded latency histograms for the entire run and samples CPU/RAM once per second,
so report memory use does not grow with the number of messages. It prints p05,
median, p95, and p99 for the modeled network path, RTL input → C++ strategy → RTL
acknowledgement, process/system CPU, and process/system RAM. CPU and RAM sample
sums and time integrals are included. It saves the report and raw one-second
resource samples under `hardware/build/performance-reports/` by default.

To use public exchange WebSockets and seed the route model with observed
per-venue interarrival jitter, add `--source websocket`. The default synthetic
run is repeatable and uses a fixed 50 μs jitter proxy; it does not measure live
WebSocket jitter.

The 152 ns RTL figure and historical 62 μs tail are reference points only. The
modeled network percentiles use assumed city routes, fiber propagation, OU jitter,
and Pareto spikes; they are not measured exchange-network latency. The software
roundtrip ends at the Verilated RTL acknowledgement and does not include an
exchange order sender.
