# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Run the recorded robot route N times and report run-to-run variation (F0-12)."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import statistics
import subprocess
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

PROJECT = Path(__file__).resolve().parents[1] / "project"
PROJECT_NAME = "Swarmstream Streaming Benchmark"
METRICS = (
    "load_ms",
    "texture_load_ms",
    "texture_read_ms",
    "avg_fps",
    "frames_over_33ms",
    "frames_over_33ms_steady",
    "frames_over_50ms",
    "frames_over_100ms",
    "excess_ms_over_33",
    "p99_frame_ms",
    "worst_frame_ms",
)
# Só métricas grandes entram no veredito: o spread percentual de valores pequenos (p99)
# e de eventos raros (excess) é ruído.
TRUST_METRICS = ("load_ms", "avg_fps")

# Limites absolutos de stutter (substituem "± 5% do baseline", que não se sustenta
# com 0–3 quadros lentos). Aprovados pelo R1 em 2026-10-18 (docs/medicoes/baseline.md §3).
# Cada limite = pior valor do baseline + margem:
#  - > 33 ms: +1. O baseline teve 1 quadro lento em 5 rodadas de 60 s (taxa ~0,2 por
#    rodada). Por Poisson, a chance de uma rodada sem regressão passar de 2 é ~0,1%.
#  - > 100 ms: +0. Nenhum no baseline; um congelamento de 100 ms é o defeito que o
#    produto promete não ter. Se aparecer, investigar a causa, não repetir até passar.
#  - p99: +2 ms (orçamento de CPU que o sw-core pode tomar; 12% de um quadro a 60 FPS).
LIMIT_MARGINS = {
    "frames_over_33ms_steady": 1.0,
    "frames_over_100ms": 0.0,
    "p99_frame_ms": 2.0,
}
# Carga: acima da média do baseline por no máximo max(10%, 500 ms), nas mesmas condições
# (quente com quente), conferida pela MEDIANA de pelo menos 3 rodadas (a carga varia ~20%
# entre rodadas; uma rodada sozinha não decide). O piso de 500 ms existe porque a carga com
# texturas GPU leva ~2 s e 10% de 2 s (200 ms) é menor que o ruído medido.
LOAD_TOLERANCE = 1.10
LOAD_TOLERANCE_MIN_MS = 500.0


def user_dir() -> Path:
    return Path(os.environ["APPDATA"]) / "Godot" / "app_userdata" / PROJECT_NAME


def run_once(godot: Path, route_seconds: float, texture_mode: str) -> tuple[dict[str, float], str]:
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
        "--tex",
        texture_mode,
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


def seconds_since_boot() -> float:
    """Segundos desde o último boot do Windows (prova da carga 'primeira após o boot')."""
    script = "((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds"
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True, check=True
    )
    return float(out.stdout.strip().replace(",", "."))


def derive_limits(runs: list[dict[str, float]]) -> dict[str, float]:
    """Limites absolutos a partir das rodadas quentes do baseline."""
    limits = {name: max(r[name] for r in runs) + margin for name, margin in LIMIT_MARGINS.items()}
    mean_load = statistics.fmean(r["load_ms"] for r in runs)
    limits["load_ms"] = max(mean_load * LOAD_TOLERANCE, mean_load + LOAD_TOLERANCE_MIN_MS)
    return limits


LOAD_MIN_RUNS = 3
REQUIRED_CHECK_METRICS = (*LIMIT_MARGINS, "load_ms")


def validate_check_metrics(metrics: object, label: str) -> list[str]:
    """Reject missing or invalid measurements before comparing any limits."""
    if not isinstance(metrics, dict):
        return [f"{label}: esperado objeto JSON com as metricas obrigatorias"]
    failures = []
    for name in REQUIRED_CHECK_METRICS:
        if name not in metrics:
            failures.append(f"{label}: metrica obrigatoria ausente: {name}")
            continue
        value = metrics[name]
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or (isinstance(value, float) and not math.isfinite(value))
            or value < 0
        ):
            failures.append(f"{label}: {name}: esperado numero finito nao negativo")
    return failures


def check_against_limits(
    reports: Sequence[Mapping[str, Any]], limits: Mapping[str, Any]
) -> list[str]:
    """Stutter: cada rodada contra o limite. Carga: a mediana das rodadas."""
    failures = validate_check_metrics(limits, "limites")
    if not reports:
        failures.append("nenhuma rodada informada")
    for index, report in enumerate(reports, start=1):
        failures.extend(validate_check_metrics(report, f"rodada {index}"))
    if failures:
        return failures

    for index, report in enumerate(reports, start=1):
        failures += [
            f"rodada {index}: {name}: {report[name]:.2f} > limite {limit:.2f}"
            for name, limit in limits.items()
            if name in LIMIT_MARGINS and report[name] > limit
        ]
    loads = [r["load_ms"] for r in reports]
    if loads:
        if len(loads) < LOAD_MIN_RUNS:
            failures.append(
                f"load_ms: {len(loads)} rodada(s); a carga exige pelo menos {LOAD_MIN_RUNS}"
            )
        elif statistics.median(loads) > limits["load_ms"]:
            failures.append(
                f"load_ms: mediana {statistics.median(loads):.2f} > limite {limits['load_ms']:.2f}"
            )
    return failures


def spread_percent(values: list[float]) -> float:
    """(max - min) / mean, in percent; 0 when the mean is 0."""
    mean = statistics.fmean(values)
    return 0.0 if mean == 0 else 100.0 * (max(values) - min(values)) / mean


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--route-seconds", type=float, default=60.0)
    parser.add_argument(
        "--warmup", type=int, default=1, help="discarded runs to warm the OS file cache"
    )
    parser.add_argument(
        "--threshold", type=float, default=10.0, help="max spread %% before the bench is untrusted"
    )
    parser.add_argument(
        "--cold",
        type=int,
        default=0,
        help="extra cold-cache runs, each after evicting the file cache",
    )
    parser.add_argument(
        "--evict-dir", type=Path, help="directory of unrelated large files read to evict the cache"
    )
    parser.add_argument("--evict-gb", type=float, default=30.0)
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--tex",
        choices=("gpu", "jpg"),
        default="gpu",
        help="gpu = BC1 pré-comprimido; jpg = método antigo",
    )
    parser.add_argument(
        "--limits-out", type=Path, help="grava os limites absolutos derivados das rodadas quentes"
    )
    parser.add_argument(
        "--check",
        type=Path,
        nargs="+",
        help="measure.json de uma ou mais rodadas, conferidos contra --limits (não roda o Godot);"
        " a carga exige pelo menos 3",
    )
    parser.add_argument("--limits", type=Path)
    parser.add_argument(
        "--after-boot",
        action="store_true",
        help="a PRIMEIRA execução é a carga fria (logo após reiniciar o Windows, sem aquecimento);"
        " depois --runs quentes",
    )
    args = parser.parse_args()

    if args.check:
        if not args.limits:
            parser.error("--check needs --limits")
        failures = check_against_limits(
            [json.loads(path.read_text(encoding="utf-8")) for path in args.check],
            json.loads(args.limits.read_text(encoding="utf-8")),
        )
        print(os.linesep.join(failures) if failures else "within limits")
        return 1 if failures else 0

    boot_cold: dict[str, float] | None = None
    boot_seconds = 0.0
    if args.after_boot:
        boot_seconds = seconds_since_boot()
        boot_cold, _ = run_once(args.godot, args.route_seconds, args.tex)
        print(
            f"cold-boot ({boot_seconds:.0f} s after boot): load_ms={boot_cold['load_ms']:.0f}",
            flush=True,
        )
    else:
        for _ in range(args.warmup):
            run_once(args.godot, args.route_seconds, args.tex)
    runs: list[dict[str, float]] = []
    hashes: set[str] = set()
    for index in range(args.runs):
        report, route_hash = run_once(args.godot, args.route_seconds, args.tex)
        runs.append(report)
        hashes.add(route_hash)
        print(f"run {index + 1}: " + ", ".join(f"{m}={report[m]:.2f}" for m in METRICS), flush=True)

    cold_runs: list[dict[str, float]] = []
    for index in range(args.cold):
        if not args.evict_dir:
            parser.error("--cold needs --evict-dir")
        left = evict_file_cache(args.evict_dir, args.evict_gb)
        report, _ = run_once(args.godot, args.route_seconds, args.tex)
        cold_runs.append(report)
        print(
            f"cold {index + 1} (evict shortfall {left} B): load_ms={report['load_ms']:.0f}",
            flush=True,
        )

    summary = {
        "renderers": sorted({r["renderer"] for r in runs}),
        "texture_mode": args.tex,
        "cold_runs": cold_runs,
        "cold_boot_run": boot_cold,
        "cold_boot_seconds_after_boot": boot_seconds if boot_cold else None,
        "limits": derive_limits(runs),
        "route_seconds": args.route_seconds,
        "route_log_identical": len(hashes) == 1,
        "runs": runs,
        "spread_percent": {m: spread_percent([r[m] for r in runs]) for m in METRICS},
    }
    print(
        json.dumps(summary["spread_percent"], indent=2),
        "route_log_identical =",
        summary["route_log_identical"],
    )
    if args.out:
        args.out.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    if args.limits_out:
        args.limits_out.write_text(json.dumps(summary["limits"], indent=2), encoding="utf-8")
    bad = [m for m in TRUST_METRICS if summary["spread_percent"][m] > args.threshold]
    if bad:
        print(f"UNTRUSTED: spread > {args.threshold}% in {bad}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
