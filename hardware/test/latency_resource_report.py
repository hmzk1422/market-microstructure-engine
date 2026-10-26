#!/usr/bin/env python3
"""Run the engine's bounded-memory, duration-based performance report."""

import argparse
import csv
from datetime import datetime, timezone
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SYSTEM_SAMPLE = re.compile(r"^\[SYSTEM\]\s+(.*)$")
KEY_VALUE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")
REQUIRED_SECTIONS = (
    "RTL-to-C++-to-RTL acknowledgement",
    "Modeled network-only round-trip latency",
    "Resource usage percentiles",
    "Paper decision counts",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path,
                        help="built market_data_engine executable")
    parser.add_argument("--duration-sec", type=int, default=3600,
                        help="test duration; default is one hour")
    parser.add_argument("--symbols", type=int, default=64,
                        help="number of synthetic assets (default: 64; ignored for WebSockets)")
    parser.add_argument("--tick-ms", type=int, default=20)
    parser.add_argument("--source", choices=("synthetic", "websocket"),
                        default="synthetic",
                        help="repeatable synthetic feed or public exchange WebSockets")
    parser.add_argument("--output-dir", type=Path,
                        help="where to save the report and raw sample CSV")
    args = parser.parse_args()
    if args.duration_sec < 1 or args.duration_sec > 86_400:
        parser.error("duration must be between 1 and 86400 seconds")
    if args.symbols < 1 or args.tick_ms < 1:
        parser.error("symbols and tick interval must be positive")

    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"engine binary does not exist: {binary}")
    output_dir = args.output_dir or (binary.parent / "performance-reports")
    output_dir.mkdir(parents=True, exist_ok=True)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    report_output = output_dir / f"performance_report_{run_id}.txt"
    trace_output = output_dir / f"resource_samples_{run_id}.csv"
    command = [
        str(binary), "--source", args.source,
        "--duration-sec", str(args.duration_sec), "--synthetic-tick-ms",
        str(args.tick_ms), "--no-telemetry",
    ]
    if args.source == "synthetic":
        symbols = ",".join(f"ASSET{i:04d}" for i in range(args.symbols))
        command.extend(("--symbols", symbols))

    with tempfile.TemporaryDirectory(prefix="market_engine_perf_") as temp_name:
        temp_dir = Path(temp_name)
        run_log = temp_dir / "engine.log"
        with run_log.open("w", encoding="utf-8") as output:
            result = subprocess.run(command, cwd=temp_dir, stdout=output,
                                    stderr=subprocess.STDOUT, check=False)
        engine_report_path = temp_dir / "market_data_engine_benchmark.txt"
        if result.returncode != 0 or not engine_report_path.is_file():
            tail = run_log.read_text(encoding="utf-8", errors="replace").splitlines()[-80:]
            print("Performance test failed; final engine output:", file=sys.stderr)
            print("\n".join(tail), file=sys.stderr)
            return result.returncode or 2

        report = engine_report_path.read_text(encoding="utf-8", errors="replace")
        missing = [section for section in REQUIRED_SECTIONS if section not in report]
        if missing:
            print(f"Engine report is missing sections: {', '.join(missing)}",
                  file=sys.stderr)
            return 2

        sample_rows = 0
        csv_output = trace_output.open("w", newline="", encoding="utf-8")
        writer = None
        with run_log.open("r", encoding="utf-8", errors="replace") as log:
            for line in log:
                match = SYSTEM_SAMPLE.match(line)
                if not match:
                    continue
                row = dict(KEY_VALUE.findall(match.group(1)))
                if writer is None:
                    writer = csv.DictWriter(csv_output, fieldnames=list(row))
                    writer.writeheader()
                writer.writerow(row)
                sample_rows += 1
        csv_output.close()
        if sample_rows == 0:
            trace_output.unlink(missing_ok=True)

    jitter_note = (
        "OU noise is seeded from live per-venue WebSocket interarrival jitter.\n"
        if args.source == "websocket" else
        "The synthetic feed supplies a 50 us jitter proxy, not live WebSocket jitter.\n"
    )
    metadata = (
        "\nPerformance test run\n"
        "====================\n"
        f"Requested duration seconds: {args.duration_sec}\n"
        f"Assets: {args.symbols if args.source == 'synthetic' else '2 (built-in default)'}\n"
        f"Synthetic tick interval ms: {args.tick_ms}\n"
        f"Input: {args.source} market feed; TimescaleDB disabled.\n"
        "Network percentiles: modeled network-only round-trip path by assumed firm location; "
        "fiber propagation plus OU jitter and Pareto tail, excluding software time. "
    ) + jitter_note + (
        "Software percentiles: Verilated RTL input through C++ strategy decision to RTL acknowledgement.\n"
        "CPU/RAM percentiles: one-second samples. CPU sample sums and percent-seconds, "
        "RAM MB sample sums and MB-seconds are reported separately.\n"
        f"Raw one-second samples CSV: {trace_output if sample_rows else 'none emitted'}\n"
    )
    report_output.write_text(report + metadata, encoding="utf-8")
    print(report + metadata)
    print(f"Saved report: {report_output.resolve()}")
    if sample_rows:
        print(f"Saved resource samples: {trace_output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
