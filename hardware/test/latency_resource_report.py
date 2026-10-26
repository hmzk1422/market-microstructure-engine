#!/usr/bin/env python3
"""Run a duration-based synthetic latency/resource report for the engine."""

import argparse
import re
import statistics
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--duration-sec", type=int, default=10)
    parser.add_argument("--symbols", type=int, default=512)
    parser.add_argument("--tick-ms", type=int, default=20)
    args = parser.parse_args()
    if args.duration_sec < 1 or args.symbols < 1 or args.tick_ms < 1:
        parser.error("duration, symbols, and tick interval must be positive")
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"engine binary does not exist: {binary}")

    symbols = ",".join(f"ASSET{i:04d}" for i in range(args.symbols))
    command = [
        str(binary), "--source", "synthetic", "--symbols", symbols,
        "--duration-sec", str(args.duration_sec), "--synthetic-tick-ms",
        str(args.tick_ms), "--no-telemetry", "--latency-every-message",
    ]
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    print("=== Engine output ===")
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        return result.returncode

    latency_rows = re.findall(
        r"verilog_cpp_verilog_roundtrip_us=([0-9.eE+-]+)", result.stdout
    )
    cpu = [float(x) for x in re.findall(r"process_cpu_pct=([0-9.eE+-]+)", result.stdout)]
    system_cpu = [float(x) for x in re.findall(r"system_cpu_pct=([0-9.eE+-]+)", result.stdout)]
    process_ram = [float(x) for x in re.findall(r"process_ram_mb=([0-9.eE+-]+)", result.stdout)]
    system_ram = [float(x) for x in re.findall(r"system_ram_used_mb=([0-9.eE+-]+)", result.stdout)]
    report_path = Path.cwd() / "market_data_engine_benchmark.txt"
    report = report_path.read_text(encoding="utf-8", errors="replace") if report_path.is_file() else ""

    print(f"runtime={args.duration_sec}s symbols={args.symbols} tick={args.tick_ms}ms samples={len(latency_rows)}")
    if latency_rows:
        values = sorted(float(x) for x in latency_rows)
        def percentile(p: float) -> float:
            return values[min(int((len(values) - 1) * p), len(values) - 1)]
        print("RTL input -> C++ strategy -> RTL acknowledgement latency (us): "
              f"p05={percentile(.05):.3f} p50={percentile(.50):.3f} "
              f"p95={percentile(.95):.3f} p99={percentile(.99):.3f} "
              f"max={values[-1]:.3f}")
        above_tail_reference = sum(value > 62.0 for value in values)
        print("Reference comparison (not pass/fail): "
              f"RTL baseline=0.152 us; p05/baseline={percentile(.05) / 0.152:.2f}x; "
              f"p95/baseline={percentile(.95) / 0.152:.2f}x; "
              f"p99/baseline={percentile(.99) / 0.152:.2f}x; "
              f">62 us historical-tail-reference={above_tail_reference}/"
              f"{len(values)} ({100.0 * above_tail_reference / len(values):.3f}%)")
    else:
        print("No per-message latency samples captured.")
    def summarize(name: str, samples: list[float], unit: str) -> None:
        if samples:
            print(f"{name}: samples={len(samples)} mean={statistics.fmean(samples):.2f}{unit} max={max(samples):.2f}{unit}")
        else:
            print(f"{name}: unavailable (no periodic sample was emitted)")
    summarize("process CPU (percent of one logical core)", cpu, "%")
    summarize("system CPU", system_cpu, "%")
    summarize("process resident RAM", process_ram, " MB")
    summarize("system used RAM", system_ram, " MB")
    print("=== Engine benchmark file ===")
    print(report or "No benchmark report was written.")
    if "pending_decisions=0" not in result.stdout or "decision_queue_dropped=0" not in result.stdout:
        print("Pipeline did not drain cleanly or dropped decisions.", file=sys.stderr)
        print(result.stdout[-4000:])
        return 2
    print("Reference only: RTL baseline 152 ns; historical tail reference 62 us. "
          "They are not pass/fail thresholds for this software-model roundtrip.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
