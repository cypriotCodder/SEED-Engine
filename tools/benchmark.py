#!/usr/bin/env python3
"""Repeat the two fixed workloads with fresh saves; no third-party Python packages required."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("executable", type=Path)
parser.add_argument("output", type=Path, help="New directory for reports and disposable benchmark saves")
parser.add_argument("--runs", type=int, default=3)
parser.add_argument("--frames", type=int, default=600)
args = parser.parse_args()
if not 1 <= args.runs <= 20 or not 1 <= args.frames <= 10000:
    parser.error("runs must be 1..20; frames must be 1..10000")
args.output.mkdir(parents=True, exist_ok=False)
summary = {}
for workload in ("static", "stream"):
    reports = []
    for run in range(1, args.runs + 1):
        report = args.output / f"{workload}-{run}.json"
        subprocess.run([str(args.executable.resolve()), "--benchmark", str(report.resolve()),
                        "--save", str((args.output / f"{workload}-save-{run}").resolve()),
                        "--workload", workload, "--frames", str(args.frames)],
                       check=True, timeout=60)
        with report.open() as source:
            reports.append(json.load(source))
    values = [r["milliseconds"]["frame_cpu"]["p95"] for r in reports]
    summary[workload] = {"runs": len(reports), "frames_per_run": args.frames,
                         "frame_cpu_p95_median_ms": statistics.median(values),
                         "frame_cpu_p95_range_ms": [min(values), max(values)],
                         "gpu_p95_median_ms": statistics.median(
                             r["milliseconds"]["render_gpu"]["p95"] for r in reports
                             if r["milliseconds"]["render_gpu"] is not None)}
with (args.output / "summary.json").open("w") as result:
    json.dump(summary, result, indent=2)
print(json.dumps(summary, indent=2))
