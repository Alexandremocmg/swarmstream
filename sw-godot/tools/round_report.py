# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Round report skeleton (F1-33): one Markdown table per network profile, with a verdict.

Reads a round file (JSON, format below) and prints a report against the Gate 1 criteria
(docs/16-briefing-fase-1a.md) and the approved stutter limits (docs/medicoes/baseline-limits.json).

Round file (schema ``swarmstream-round-v0``)::

    {
      "schema": "swarmstream-round-v0",          # optional; when present it must match
      "round_id": "2026-10-19-cabo",             # optional, free text
      "profiles": {
        "cabo_50": {
          "runs": [                               # one entry per run; the load needs >= 3 runs
            {
              "load_ms": 2296.4,                  # fields baseline.py already writes:
              "frames_over_33ms_steady": 0,       #   load_ms, frames_over_33ms_steady,
              "frames_over_100ms": 0,             #   frames_over_100ms, p99_frame_ms
              "p99_frame_ms": 3.7,
              "telemetry": {                      # optional until F1-30 exists; absent = "n/d"
                                                  # window = robot route, SW_GAME_STATE_PLAYING
                "reads": 1200,                    # delta of sw_stats.reads over the window
                "read_misses": 7,                 # delta of sw_stats.read_misses over the window
                "miss_start_events": 7,           # read_miss_start with t0 in the window; must
                                                  #   equal read_misses (else event loss)
                "miss_wait_us": [12000, 80000],   # miss_wait_us of read_miss, outcome=completed
                "failed": 0,                      # outcome=failed
                "cancelled": 0,                   # outcome=cancelled
                "pending_at_end": 0               # outcome=pending_at_end (after the 30 s drain)
              }
            }
          ]
        }
      }
    }

Rule of the report: a missing or invalid metric is shown as ``n/d`` and the verdict is
``incompleto``; it is never ``passou`` (the lesson of F1-36). Exit code 0 only when every
required profile passed; 1 for ``falhou`` or ``incompleto``; 2 for unreadable input.
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

import baseline

SCHEMA = "swarmstream-round-v0"
REQUIRED_PROFILES = ("fibra_300", "cabo_50", "adsl_10")
GATE_PROFILE = "cabo_50"  # the Gate 1 numbers (miss rate, wait p99) are decided only here
MISS_RATE_LIMIT_PERCENT = 1.0  # docs/16: taxa de falta < 1%
WAIT_P99_LIMIT_MS = 300.0  # docs/16: espera numa falta, p99 < 300 ms
MIN_LOAD_RUNS = baseline.LOAD_MIN_RUNS
STUTTER_METRICS = tuple(baseline.LIMIT_MARGINS)

ND = "n/d"
OK = "ok"
FAIL = "falhou"
INFO = "info"
PENDING = "incompleto"
CUMPRE = "cumpre"  # flags on the per-round / pooled rows; the Portão 1 row decides
NAO_CUMPRE = "não cumpre"
AGG_NOTE = (
    "agregacao das rodadas no Portao 1 pendente do dono do produto (docs/08): "
    "todas as rodadas passam, ou vale o valor conjunto? As duas regras discordam"
)
UNFINISHED_KEYS = ("failed", "cancelled", "pending_at_end")
COUNT_KEYS = ("reads", "read_misses", "miss_start_events", *UNFINISHED_KEYS)


@dataclass
class Row:
    metric: str
    value: str
    criterion: str
    status: str  # ok | falhou | n/d | info | incompleto


@dataclass
class ProfileResult:
    name: str
    rows: list[Row] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    runs: int = 0
    verdict: str = PENDING


@dataclass
class RoundResult:
    round_id: str
    profiles: list[ProfileResult]
    verdict: str


def nearest_rank(values: Sequence[float], p: float) -> float:
    """Nearest-rank percentile (docs/08): sorted values, index ceil(p * N) (1-based)."""
    ordered = sorted(values)
    return ordered[max(math.ceil(p * len(ordered)), 1) - 1]


def _is_count(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _is_wait(value: object) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value >= 0
    )


def validate_telemetry(telemetry: object, label: str) -> list[str]:
    """Reasons a run's telemetry block cannot be trusted (empty list = valid)."""
    if not isinstance(telemetry, Mapping):
        return [f"{label}: telemetria ausente"]
    reasons = []
    for key in COUNT_KEYS:
        if not _is_count(telemetry.get(key)):
            reasons.append(f"{label}: telemetria.{key}: esperado inteiro nao negativo")
    waits = telemetry.get("miss_wait_us")
    wait_list: list[Any] = waits if isinstance(waits, list) else []
    if not isinstance(waits, list) or not all(_is_wait(w) for w in wait_list):
        reasons.append(f"{label}: telemetria.miss_wait_us: esperado lista de numeros finitos >= 0")
    if reasons:
        return reasons
    reads, misses = telemetry["reads"], telemetry["read_misses"]
    if reads == 0:
        reasons.append(f"{label}: telemetria.reads = 0 (sem leituras nao ha taxa de falta)")
    if misses > reads:
        reasons.append(f"{label}: telemetria.read_misses > reads")
    unfinished = sum(telemetry[key] for key in UNFINISHED_KEYS)
    if len(wait_list) + unfinished != misses:
        reasons.append(
            f"{label}: telemetria: len(miss_wait_us) + failed + cancelled + pending_at_end"
            " deve ser igual a read_misses"
        )
    if telemetry["miss_start_events"] != misses:
        reasons.append(
            f"{label}: perda de evento: miss_start_events ({telemetry['miss_start_events']})"
            f" difere de read_misses ({misses})"
        )
    return reasons


def _format_ms(value: float) -> str:
    return f"{value:.2f}"


def _stutter_rows(
    valid_runs: Sequence[Mapping[str, Any]], limits: Mapping[str, Any], total_runs: int
) -> list[Row]:
    rows: list[Row] = []
    labels = {
        "frames_over_33ms_steady": "Quadros > 33 ms (após 3 s), pior rodada",
        "frames_over_100ms": "Quadros > 100 ms, pior rodada",
        "p99_frame_ms": "p99 do quadro (ms), pior rodada",
    }
    for name in STUTTER_METRICS:
        criterion = f"<= {limits[name]:g}"
        if not valid_runs:
            rows.append(Row(labels[name], ND, criterion, ND))
            continue
        worst = max(float(run[name]) for run in valid_runs)
        status = FAIL if worst > limits[name] else OK
        if len(valid_runs) < total_runs and status == OK:
            status = PENDING  # some runs were invalid: cannot call it ok
        rows.append(Row(labels[name], f"{worst:g}", criterion, status))
    load_criterion = f"mediana <= {limits['load_ms']:g} ms (>= {MIN_LOAD_RUNS} rodadas)"
    if len(valid_runs) < MIN_LOAD_RUNS:
        rows.append(Row("Carga (ms)", ND, load_criterion, ND))
    else:
        median = statistics.median(float(run["load_ms"]) for run in valid_runs)
        status = FAIL if median > limits["load_ms"] else OK
        if len(valid_runs) < total_runs and status == OK:
            status = PENDING
        rows.append(Row("Carga (ms)", f"{median:g}", load_criterion, status))
    return rows


def _aggregate_status(per_round_ok: Sequence[bool], pooled_ok: bool, notes: list[str]) -> str:
    """Gate 1 aggregation is the owner's decision (docs/08): pass only when both rules agree."""
    if all(per_round_ok) and pooled_ok:
        return OK
    if not all(per_round_ok) and not pooled_ok:
        return FAIL
    if AGG_NOTE not in notes:
        notes.append(AGG_NOTE)
    return PENDING


def _telemetry_rows(name: str, runs: Sequence[Any], notes: list[str]) -> list[Row]:
    is_gate = name == GATE_PROFILE
    rate_criterion = f"< {MISS_RATE_LIMIT_PERCENT:g}%" if is_gate else "informativo"
    wait_criterion = f"p99 < {WAIT_P99_LIMIT_MS:g} ms" if is_gate else "informativo"
    nd_rows = [
        Row("Taxa de falta, por rodada", ND, rate_criterion, ND),
        Row("Taxa de falta, rodadas juntas", ND, rate_criterion, ND),
        Row("Espera numa falta p99 por rodada (ms)", ND, wait_criterion, ND),
        Row("Espera numa falta p50/p95/p99, rodadas juntas (ms)", ND, wait_criterion, ND),
        Row("Faltas não concluídas (failed / cancelled / pending_at_end)", ND, "= 0", ND),
    ]
    blocks = [run.get("telemetry") if isinstance(run, Mapping) else None for run in runs]
    reasons = [
        reason
        for index, block in enumerate(blocks, start=1)
        for reason in validate_telemetry(block, f"rodada {index}")
    ]
    if reasons or not blocks:
        notes.extend(reasons)
        return nd_rows
    valid: list[Mapping[str, Any]] = [block for block in blocks if isinstance(block, Mapping)]
    rates = [100.0 * b["read_misses"] / b["reads"] for b in valid]
    reads = sum(b["reads"] for b in valid)
    misses = sum(b["read_misses"] for b in valid)
    pooled_rate = 100.0 * misses / reads
    round_waits = [[float(w) / 1000.0 for w in b["miss_wait_us"]] for b in valid]
    all_waits = [w for waits in round_waits for w in waits]
    p99s = [nearest_rank(w, 0.99) if w else 0.0 for w in round_waits]
    pooled_p99 = nearest_rank(all_waits, 0.99) if all_waits else 0.0
    rate_flags = [rate < MISS_RATE_LIMIT_PERCENT for rate in rates]
    wait_flags = [p99 < WAIT_P99_LIMIT_MS for p99 in p99s]
    rate_pooled_ok = pooled_rate < MISS_RATE_LIMIT_PERCENT
    wait_pooled_ok = pooled_p99 < WAIT_P99_LIMIT_MS

    def flag(ok: bool) -> str:
        return (CUMPRE if ok else NAO_CUMPRE) if is_gate else INFO

    if all_waits:
        p50, p95 = nearest_rank(all_waits, 0.50), nearest_rank(all_waits, 0.95)
        pooled_value = (
            f"{_format_ms(p50)} / {_format_ms(p95)} / {_format_ms(pooled_p99)} (N={len(all_waits)})"
        )
    else:
        pooled_value = "sem faltas concluídas (N=0)"
    rows = [
        Row(
            "Taxa de falta, por rodada",
            " · ".join(f"{rate:.3f}%" for rate in rates),
            f"{rate_criterion} em cada rodada" if is_gate else rate_criterion,
            flag(all(rate_flags)),
        ),
        Row(
            "Taxa de falta, rodadas juntas",
            f"{pooled_rate:.3f}% ({misses}/{reads})",
            rate_criterion,
            flag(rate_pooled_ok),
        ),
    ]
    if is_gate:
        rows.append(
            Row(
                "Taxa de falta (Portão 1)",
                "—",
                "as duas regras concordam",
                _aggregate_status(rate_flags, rate_pooled_ok, notes),
            )
        )
    rows += [
        Row(
            "Espera numa falta p99 por rodada (ms)",
            " · ".join(_format_ms(p99) for p99 in p99s),
            f"{wait_criterion} em cada rodada" if is_gate else wait_criterion,
            flag(all(wait_flags)),
        ),
        Row(
            "Espera numa falta p50/p95/p99, rodadas juntas (ms)",
            pooled_value,
            wait_criterion,
            flag(wait_pooled_ok),
        ),
    ]
    if is_gate:
        rows.append(
            Row(
                "Espera numa falta p99 (Portão 1)",
                "—",
                "as duas regras concordam",
                _aggregate_status(wait_flags, wait_pooled_ok, notes),
            )
        )
    counts = [sum(b[key] for b in valid) for key in UNFINISHED_KEYS]
    if any(counts):
        notes.append(
            f"{sum(counts)} falta(s) não concluída(s) na janela (failed/cancelled/pending_at_end):"
            " a rodada fica incompleta (docs/08)"
        )
    rows.append(
        Row(
            "Faltas não concluídas (failed / cancelled / pending_at_end)",
            " / ".join(str(c) for c in counts),
            "= 0",
            PENDING if any(counts) else OK,
        )
    )
    return rows


def evaluate_profile(name: str, profile: object, limits: Mapping[str, Any]) -> ProfileResult:
    result = ProfileResult(name=name)
    runs = profile.get("runs") if isinstance(profile, Mapping) else None
    if not isinstance(runs, list) or not runs:
        result.notes.append("sem rodadas")
        result.rows = _stutter_rows([], limits, 0) + _telemetry_rows(name, [], result.notes)
        return result
    result.runs = len(runs)
    valid_runs = []
    for index, run in enumerate(runs, start=1):
        problems = baseline.validate_check_metrics(run, f"rodada {index}")
        if problems:
            result.notes.extend(problems)
        else:
            valid_runs.append(run)
    if len(valid_runs) < MIN_LOAD_RUNS:
        result.notes.append(
            f"{len(valid_runs)} rodada(s) valida(s); a carga exige pelo menos {MIN_LOAD_RUNS}"
        )
    result.rows = _stutter_rows(valid_runs, limits, len(runs)) + _telemetry_rows(
        name, runs, result.notes
    )
    statuses = {row.status for row in result.rows}
    if FAIL in statuses:
        result.verdict = FAIL
    elif statuses - {CUMPRE, NAO_CUMPRE} <= {OK, INFO}:
        result.verdict = "passou"
    else:
        result.verdict = PENDING
    return result


def evaluate_round(report: Mapping[str, Any], limits: Mapping[str, Any]) -> RoundResult:
    profiles = report.get("profiles")
    profiles = profiles if isinstance(profiles, Mapping) else {}
    names = list(REQUIRED_PROFILES) + sorted(n for n in profiles if n not in REQUIRED_PROFILES)
    results = []
    for name in names:
        if name in profiles:
            results.append(evaluate_profile(name, profiles[name], limits))
        else:
            results.append(ProfileResult(name=name, notes=["perfil ausente"]))
    if any(p.verdict == FAIL for p in results):
        verdict = FAIL
    elif all(p.verdict == "passou" for p in results if p.name in REQUIRED_PROFILES):
        verdict = "passou"
    else:
        verdict = PENDING
    return RoundResult(
        round_id=str(report.get("round_id", "sem id")), profiles=results, verdict=verdict
    )


def render_markdown(result: RoundResult) -> str:
    lines = [
        f"# Relatório da rodada {result.round_id}",
        "",
        f"**Portão 1 (automático): {result.verdict}**",
        "",
        "Critérios: taxa de falta < 1% e espera numa falta p99 < 300 ms (decididos em `cabo_50`); "
        "stutter e carga dentro dos limites absolutos aprovados em todos os perfis. "
        "`n/d` = métrica ausente ou inválida; a rodada fica `incompleto`, nunca `passou`.",
        "",
        "Janela da telemetria: rota do robô com `SW_GAME_STATE_PLAYING`, carga inicial à parte; "
        "taxa e p99 por rodada e com as rodadas juntas (docs/08). Agregação no Portão 1: "
        "**pendente do dono do produto**; só dá `passou` quando as duas regras concordam.",
        "",
        "Pop-in: avaliação visual manual, fora deste relatório (registrar à parte).",
        "",
        "| Perfil | Rodadas | Veredito |",
        "| --- | --- | --- |",
    ]
    lines += [f"| {p.name} | {p.runs} | {p.verdict} |" for p in result.profiles]
    for profile in result.profiles:
        lines += ["", f"## {profile.name}: {profile.verdict}"]
        if profile.rows:
            lines += ["", "| Métrica | Valor | Critério | Situação |", "| --- | --- | --- | --- |"]
            lines += [
                f"| {r.metric} | {r.value} | {r.criterion} | {r.status} |" for r in profile.rows
            ]
        if profile.notes:
            lines += ["", "Motivos:"] + [f"- {note}" for note in profile.notes]
    return "\n".join(lines) + "\n"


def _load_json(path: Path, what: str) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise SystemExit(f"{what}: nao foi possivel ler {path}: {error}") from error


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Round report: Markdown table and verdict per profile"
    )
    parser.add_argument("round_file", type=Path, help="round file (JSON, schema above)")
    parser.add_argument("--limits", type=Path, required=True, help="baseline-limits.json")
    parser.add_argument("--out", type=Path, help="write the Markdown report here too")
    args = parser.parse_args(argv)
    try:
        report = _load_json(args.round_file, "rodada")
        limits = _load_json(args.limits, "limites")
    except SystemExit as error:
        print(error, file=sys.stderr)
        return 2
    if not isinstance(report, dict):
        print("rodada: esperado objeto JSON", file=sys.stderr)
        return 2
    if "schema" in report and report["schema"] != SCHEMA:
        print(
            f"rodada: schema {report['schema']!r} desconhecido (esperado {SCHEMA})", file=sys.stderr
        )
        return 2
    problems = baseline.validate_check_metrics(limits, "limites")
    if problems or not isinstance(limits, dict):
        print("\n".join(problems), file=sys.stderr)
        return 2
    result = evaluate_round(report, limits)
    text = render_markdown(result)
    sys.stdout.write(text)
    if args.out:
        args.out.write_text(text, encoding="utf-8", newline="\n")
    return 0 if result.verdict == "passou" else 1


if __name__ == "__main__":
    raise SystemExit(main())
