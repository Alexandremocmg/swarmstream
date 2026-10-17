# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Run the recorded robot route N times and report run-to-run variation (F0-12)."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1] / "project"
PROJECT_NAME = "Swarmstream Streaming Benchmark"
METRICS = (
    "load_ms",
    "avg_fps",
    "frames_over_33ms",
    "frames_over_33ms_steady",
    "excess_ms_over_33",
    "p99_frame_ms",
    "worst_frame_ms",
)
# Só métricas grandes entram no veredito: spread percentual de valores pequenos (p99 ~4 ms) e de eventos raros (excess) é ruído.
TRUST_METRICS = ("load_ms", "avg_fps")


def user_dir() -> Path:
    return Path(os.environ["APPDATA"]) / "Godot" / "app_userdata" / PROJECT_NAME


def run_once(godot: Path, route_seconds: float) -> tuple[dict[str, float], str]:
    for name in ("measure.json", "route.log"):
        (user_dir() / name).unlink(missing_ok=True)
    command = [
        str(godot),
        "--path",
        str(PROJECT),
        "--disable-vsync",
        "--",
        "--robot",
        "--measure",
        "--route-seconds",
        str(route_seconds),
    ]
    subprocess.run(command, check=True, capture_output=True, timeout=route_seconds + 120)
    report = json.loads((user_dir() / "measure.json").read_text(encoding="utf-8"))
    route_hash = hashlib.sha256((user_dir() / "route.log").read_bytes()).hexdigest()
    return report, route_hash


def evict_file_cache(evict_dir: Path, gigabytes: float) -> int:
    """Read unrelated files so the OS drops our assets from its file cache (no admin needed)."""
    remaining = int(gigabytes * 1024**3)
    for path in sorted(evict_dir.rglob("*"), key=lambda p: -p.stat().st_size if p.is_file() else 0):
        if remaining <= 0 or not path.is_file():
            continue
        try:
            with path.open("rb") as handle:
                while remaining > 0 and (chunk := handle.read(16 * 1024 * 1024)):
                    remaining -= len(chunk)
        except OSError:
            continue
    return max(remaining, 0)


def spread_percent(values: list[float]) -> float:
    """(max - min) / mean, in percent; 0 when the mean is 0."""
    mean = statistics.fmean(values)
    return 0.0 if mean == 0 else 100.0 * (max(values) - min(values)) / mean


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--route-seconds", type=float, default=60.0)
    parser.add_argument("--warmup", type=int, default=1, help="discarded runs to warm the OS file cache")
    parser.add_argument("--threshold", type=float, default=10.0, help="max spread %% before the bench is untrusted")
    parser.add_argument("--cold", type=int, default=0, help="extra cold-cache runs, each after evicting the file cache")
    parser.add_argument("--evict-dir", type=Path, help="directory of unrelated large files read to evict the cache")
    parser.add_argument("--evict-gb", type=float, default=30.0)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    for _ in range(args.warmup):
        run_once(args.godot, args.route_seconds)
    runs: list[dict[str, float]] = []
    hashes: set[str] = set()
    for index in range(args.runs):
        report, route_hash = run_once(args.godot, args.route_seconds)
        runs.append(report)
        hashes.add(route_hash)
        print(f"run {index + 1}: " + ", ".join(f"{m}={report[m]:.2f}" for m in METRICS), flush=True)

    cold_runs: list[dict[str, float]] = []
    for index in range(args.cold):
        if not args.evict_dir:
            parser.error("--cold needs --evict-dir")
        left = evict_file_cache(args.evict_dir, args.evict_gb)
        report, _ = run_once(args.godot, args.route_seconds)
        cold_runs.append(report)
        print(f"cold {index + 1} (evict shortfall {left} B): load_ms={report['load_ms']:.0f}", flush=True)

    summary = {
        "renderers": sorted({r["renderer"] for r in runs}),
        "cold_runs": cold_runs,
        "route_seconds": args.route_seconds,
        "route_log_identical": len(hashes) == 1,
        "runs": runs,
        "spread_percent": {m: spread_percent([r[m] for r in runs]) for m in METRICS},
    }
    print(json.dumps(summary["spread_percent"], indent=2), "route_log_identical =", summary["route_log_identical"])
    if args.out:
        args.out.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    bad = [m for m in TRUST_METRICS if summary["spread_percent"][m] > args.threshold]
    if bad:
        print(f"UNTRUSTED: spread > {args.threshold}% in {bad}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
