# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Known-change validation of the bench (F1-47): a slower network must worsen the numbers.

Compares two profiles of a round file (schema ``swarmstream-round-v0``, see round_report.py),
by default ``cabo_50`` (fast) against ``adsl_10`` (slow), and says whether the bench follows the
change. Nothing here measures anything: it reads files produced by a round, and the round can
only exist once the game reads through the network (F1-27 HTTP source, F1-20 Godot loader).

Each profile needs >= 3 runs. Per run: ``load_ms`` and the ``telemetry`` block validated by
``round_report.validate_telemetry``. Each profile also carries an ``environment`` object that must
be IDENTICAL in both profiles (same machine, GPU, build and route; docs/16)::

    "environment": {"machine": "...", "gpu": "...", "build": "...", "route_sha256": "..."}

Verdicts (exit code 0 only for ``coerente``):
- ``coerente``   the slow profile is worse in the waits and/or the load, and better in neither.
- ``nao_mede``   neither worsens: the bench does not follow the network (risk R15); Gate 1 cannot
                 be decided.
- ``incoerente`` the slow profile is BETTER in some metric: something else is moving the numbers.
- ``incompleto`` missing or invalid data, environments that differ, fewer samples than the
                 minimum, or no minimum given (the minimum is pending F1-48; pass --min-samples).
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import round_report

FAST_PROFILE = "cabo_50"
SLOW_PROFILE = "adsl_10"
# Mirrors sw-netlab/server.py PROFILES: (bandwidth Mbps, latency ms). A test checks they match.
NETWORK = {"cabo_50": (50.0, 25.0), "adsl_10": (10.0, 60.0), "fibra_300": (300.0, 10.0)}
PIECE_BYTES = 1 << 20  # a miss moves at least one piece of up to 1 MiB (docs/03)
# Proposals, to be confirmed by R1 / the owner before the final measurements (docs/medicoes).
DEFAULT_MIN_WAIT_RATIO = 2.0  # expected ~4.7x for a 1 MiB piece; half of it, to allow noise
DEFAULT_MIN_LOAD_RATIO = 1.2
DEFAULT_NOISE = 0.10  # run-to-run spread the bench tolerates (baseline.py threshold, 10%)
SURPRISE_FACTOR = 3.0  # a ratio above this many times the expected one is flagged, not failed
MIN_RUNS = 3
ENVIRONMENT_KEYS = ("machine", "gpu", "build", "route_sha256")

COHERENT = "coerente"
NOT_MEASURING = "nao_mede"
INCOHERENT = "incoerente"
INCOMPLETE = "incompleto"
ND = "n/d"


@dataclass
class Row:
    metric: str
    fast: str
    slow: str
    ratio: str
    reading: str  # piora | igual | melhora | n/d


@dataclass
class Comparison:
    fast: str
    slow: str
    rows: list[Row] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    expected_ratio: float = 0.0
    verdict: str = INCOMPLETE


def expected_wait_ms(profile: str, piece_bytes: int = PIECE_BYTES) -> float:
    """Latency plus the time to move one piece at the profile bandwidth."""
    bandwidth_mbps, latency_ms = NETWORK[profile]
    return latency_ms + piece_bytes * 8 / (bandwidth_mbps * 1e6) * 1000.0


def ratio(slow: float, fast: float) -> float:
    if fast > 0:
        return slow / fast
    return math.inf if slow > 0 else 1.0


def _format_ratio(value: float) -> str:
    return "∞" if math.isinf(value) else f"{value:.2f}x"


def _reading(value: float, minimum: float, noise: float) -> str:
    """piora above ``minimum``; melhora only below 1 - noise (inside the noise it is igual)."""
    if value >= minimum:
        return "piora"
    if value < 1.0 - noise:
        return "melhora"
    return "igual"


def _runs(report: Mapping[str, Any], name: str) -> list[Any] | None:
    profiles = report.get("profiles")
    profile = profiles.get(name) if isinstance(profiles, Mapping) else None
    runs = profile.get("runs") if isinstance(profile, Mapping) else None
    return runs if isinstance(runs, list) and runs else None


def _environment(report: Mapping[str, Any], name: str) -> object:
    profiles = report.get("profiles")
    profile = profiles.get(name) if isinstance(profiles, Mapping) else None
    return profile.get("environment") if isinstance(profile, Mapping) else None


def _collect(
    runs: Sequence[Any], label: str, notes: list[str]
) -> tuple[list[float], list[float], bool]:
    """(waits in ms of every completed miss, load_ms per run, data_ok); problems go to ``notes``."""
    waits: list[float] = []
    loads: list[float] = []
    data_ok = True
    for index, run in enumerate(runs, start=1):
        where = f"{label} rodada {index}"
        if not isinstance(run, Mapping):
            notes.append(f"{where}: esperado objeto JSON")
            data_ok = False
            continue
        load = run.get("load_ms")
        if isinstance(load, bool) or not isinstance(load, (int, float)):
            notes.append(f"{where}: load_ms: esperado numero")
            data_ok = False
        elif not math.isfinite(load) or load < 0:
            notes.append(f"{where}: load_ms: esperado numero finito nao negativo")
            data_ok = False
        else:
            loads.append(float(load))
        telemetry = run.get("telemetry")
        problems = round_report.validate_telemetry(telemetry, where)
        if problems:
            notes.extend(problems)
            data_ok = False
        elif isinstance(telemetry, Mapping):
            waits.extend(float(w) / 1000.0 for w in telemetry["miss_wait_us"])
            unfinished = sum(telemetry[key] for key in round_report.UNFINISHED_KEYS)
            if unfinished:
                notes.append(f"{where}: {unfinished} falta(s) nao concluida(s): rodada incompleta")
                data_ok = False
    return waits, loads, data_ok


def compare(
    report: Mapping[str, Any],
    fast: str = FAST_PROFILE,
    slow: str = SLOW_PROFILE,
    min_samples: int | None = None,
    min_wait_ratio: float = DEFAULT_MIN_WAIT_RATIO,
    min_load_ratio: float = DEFAULT_MIN_LOAD_RATIO,
    noise: float = DEFAULT_NOISE,
) -> Comparison:
    result = Comparison(fast=fast, slow=slow)
    if fast in NETWORK and slow in NETWORK:
        result.expected_ratio = expected_wait_ms(slow) / expected_wait_ms(fast)
    blocked = False
    runs = {name: _runs(report, name) for name in (fast, slow)}
    for name, profile_runs in runs.items():
        if profile_runs is None:
            result.notes.append(f"perfil {name} ausente ou sem rodadas")
            blocked = True
        elif len(profile_runs) < MIN_RUNS:
            result.notes.append(
                f"{name}: {len(profile_runs)} rodada(s); a comparacao exige {MIN_RUNS}"
            )
            blocked = True
    fast_runs, slow_runs = runs[fast], runs[slow]
    if fast_runs is None or slow_runs is None:
        return result
    envs = (_environment(report, fast), _environment(report, slow))
    if not all(isinstance(e, Mapping) and all(k in e for k in ENVIRONMENT_KEYS) for e in envs):
        result.notes.append(
            f"environment ausente ou sem {'/'.join(ENVIRONMENT_KEYS)} em algum perfil"
        )
        blocked = True
    elif envs[0] != envs[1]:
        result.notes.append("environment difere entre os perfis: rodadas nao comparaveis")
        blocked = True
    waits_fast, loads_fast, ok_fast = _collect(fast_runs, fast, result.notes)
    waits_slow, loads_slow, ok_slow = _collect(slow_runs, slow, result.notes)
    blocked = blocked or not (ok_fast and ok_slow)
    if min_samples is None:
        result.notes.append(
            "minimo de amostras pendente do F1-48; "
            "informe --min-samples para uma decisao provisoria"
        )
        blocked = True
    else:
        for name, waits in ((fast, waits_fast), (slow, waits_slow)):
            if len(waits) < min_samples:
                result.notes.append(
                    f"{name}: {len(waits)} falta(s) concluida(s), minimo {min_samples}"
                )
                blocked = True
    if not waits_fast or not waits_slow or min(len(loads_fast), len(loads_slow)) < MIN_RUNS:
        result.rows = [
            Row("Espera numa falta p50 (ms)", ND, ND, ND, ND),
            Row("Carga, mediana (ms)", ND, ND, ND, ND),
        ]
        return result
    thresholds = (min_wait_ratio, min_load_ratio, noise)
    return _finish(result, blocked, (waits_fast, waits_slow), (loads_fast, loads_slow), thresholds)


def _finish(
    result: Comparison,
    blocked: bool,
    waits: tuple[list[float], list[float]],
    loads: tuple[list[float], list[float]],
    thresholds: tuple[float, float, float],
) -> Comparison:
    min_wait_ratio, min_load_ratio, noise = thresholds
    wait_fast, wait_slow = waits
    p50_reading = ""
    any_better = False
    for label, p in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99)):
        a, b = round_report.nearest_rank(wait_fast, p), round_report.nearest_rank(wait_slow, p)
        r = ratio(b, a)
        reading = _reading(r, min_wait_ratio if p == 0.50 else 1.0 + noise, noise)
        if p == 0.50:
            p50_reading = reading
            if result.expected_ratio and r > SURPRISE_FACTOR * result.expected_ratio:
                result.notes.append(
                    f"razao do p50 ({_format_ratio(r)}) acima de {SURPRISE_FACTOR:g}x a esperada "
                    f"({result.expected_ratio:.2f}x): investigar "
                    "(tentativas por perda? outra fonte de espera?)"
                )
        any_better = any_better or reading == "melhora"
        result.rows.append(
            Row(
                f"Espera numa falta {label} (ms)",
                f"{a:.2f} (N={len(wait_fast)})",
                f"{b:.2f} (N={len(wait_slow)})",
                _format_ratio(r),
                reading,
            )
        )
    load_fast, load_slow = statistics.median(loads[0]), statistics.median(loads[1])
    load_ratio = ratio(load_slow, load_fast)
    load_reading = _reading(load_ratio, min_load_ratio, noise)
    any_better = any_better or load_reading == "melhora"
    result.rows.append(
        Row(
            "Carga, mediana (ms)",
            f"{load_fast:.2f}",
            f"{load_slow:.2f}",
            _format_ratio(load_ratio),
            load_reading,
        )
    )
    if blocked:
        result.verdict = INCOMPLETE
    elif any_better:
        result.verdict = INCOHERENT
    elif p50_reading == "piora" or load_reading == "piora":
        result.verdict = COHERENT
    else:
        result.verdict = NOT_MEASURING
    return result


def render_markdown(result: Comparison) -> str:
    expected = (
        f"{result.expected_ratio:.2f}x (latência + 1 MiB / banda, de `sw-netlab`)"
        if result.expected_ratio
        else "n/d"
    )
    lines = [
        f"# Mudança conhecida: {result.fast} (rápido) contra {result.slow} (lento)",
        "",
        f"**Veredito: {result.verdict}**",
        "",
        "`coerente` = o perfil lento piora a espera e/ou a carga e não melhora nenhuma; "
        "`nao_mede` = nada piora (a bancada não acompanha a rede; risco R15; o Portão 1 não pode "
        "ser decidido); `incoerente` = algo melhora no perfil lento; `incompleto` = dado faltando, "
        "inválido, ambientes diferentes ou amostras abaixo do mínimo.",
        "",
        f"Razão esperada da espera (1 peça de 1 MiB): {expected}.",
        "",
        "| Métrica | " + result.fast + " | " + result.slow + " | Razão lento/rápido | Leitura |",
        "| --- | --- | --- | --- | --- |",
    ]
    lines += [
        f"| {r.metric} | {r.fast} | {r.slow} | {r.ratio} | {r.reading} |" for r in result.rows
    ]
    if result.notes:
        lines += ["", "Motivos e avisos:"] + [f"- {note}" for note in result.notes]
    return "\n".join(lines) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Known-change validation of the bench (F1-47)")
    parser.add_argument("round_file", type=Path)
    parser.add_argument("--fast", default=FAST_PROFILE)
    parser.add_argument("--slow", default=SLOW_PROFILE)
    parser.add_argument("--min-samples", type=int, help="completed misses per profile (F1-48)")
    parser.add_argument("--min-wait-ratio", type=float, default=DEFAULT_MIN_WAIT_RATIO)
    parser.add_argument("--min-load-ratio", type=float, default=DEFAULT_MIN_LOAD_RATIO)
    parser.add_argument("--noise", type=float, default=DEFAULT_NOISE)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    try:
        report = json.loads(args.round_file.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"rodada: nao foi possivel ler {args.round_file}: {error}", file=sys.stderr)
        return 2
    if not isinstance(report, dict):
        print("rodada: esperado objeto JSON", file=sys.stderr)
        return 2
    if "schema" in report and report["schema"] != round_report.SCHEMA:
        print(f"rodada: schema {report['schema']!r} desconhecido", file=sys.stderr)
        return 2
    result = compare(
        report,
        args.fast,
        args.slow,
        args.min_samples,
        args.min_wait_ratio,
        args.min_load_ratio,
        args.noise,
    )
    text = render_markdown(result)
    sys.stdout.write(text)
    if args.out:
        args.out.write_text(text, encoding="utf-8", newline="\n")
    return 0 if result.verdict == COHERENT else 1


if __name__ == "__main__":
    raise SystemExit(main())
