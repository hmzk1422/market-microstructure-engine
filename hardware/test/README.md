# Hardware tests and performance report

Build and run correctness tests:

```sh
cmake -S hardware -B hardware/build -DCMAKE_BUILD_TYPE=Release
cmake --build hardware/build --parallel
ctest --test-dir hardware/build --output-on-failure
```

Run the duration-based, 512-symbol latency/resource report (Linux/WSL, Python 3):

```sh
python3 hardware/test/latency_resource_report.py --binary hardware/build/market_data_engine --duration-sec 10 --symbols 512
```

The report captures every RTL input-to-RTL acknowledgement sample, p05/p50/p95/p99/max, periodic process/system CPU and RAM samples, and the engine's benchmark summary. `--latency-every-message` intentionally adds console overhead; use the normal CTest run or omit it for lower-overhead performance numbers.

152 ns is retained as a reference RTL baseline and 62 us as a historical tail-spike reference only. Neither is a pass/fail threshold for the Verilated software pipeline: its measured roundtrip includes host scheduling, the C++ strategy worker, and the shared-memory queue. It is not a physical FPGA or network measurement.
