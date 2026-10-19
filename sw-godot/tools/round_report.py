# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Round report (F1-33): one Markdown table per network profile, with a verdict.

Reads a round file (JSON, format below) and prints a report against the Gate 1 rule decided by
the product owner on 2026-10-19 (docs/16-briefing-fase-1a.md, docs/08-metricas-e-testes.md) and the
approved stutter limits (docs/medicoes/baseline-limits.json).

Gate 1 rule, in ``cabo_50`` (the other profiles are measured and reported, not decided):
- miss rate < 1% in EACH round (never the average of the rounds);
- zero misses in logic and collision (misses on ``core`` files; how to count them exactly is
  part of F1-48);
- wait p99 < 300 ms over the INDIVIDUAL miss events pooled from at least 3 complete and
  comparable rounds (never the average of the round p99s);
- no misses is ``n/d``, never p99 = 0; pooled samples below the minimum are ``incompleto``. The
  minimum comes from F1-48 and is configurable (``--min-samples``); without it the pooled p99 is
  ``incompleto`` ("mínimo pendente do F1-48").
Automatic metrics that pass do NOT approve Gate 1 alone: the recorded visual pop-in evaluation is
still required.

Round file (schema ``swarmstream-round-v0``)::

    {
      "schema": "swarmstream-round-v0",          # optional; when present it must match
      "round_id": "2026-10-19-cabo",             # optional, free text
      "profiles": {
        "cabo_50": {
          "environment": {"machine": "...", "gpu": "...",   # optional here if each run has one;
                          "build": "...", "route_sha256": "..."},  # must be equal in all runs
          "runs": [                               # one entry per run; the load needs >= 3 runs
            {
              "load_ms": 2296.4,                  # fields baseline.py already writes:
              "frames_over_33ms_steady": 0,       #   load_ms, frames_over_33ms_steady,
              "frames_over_100ms": 0,             #   frames_over_100ms, p99_frame_ms
              "p99_frame_ms": 3.7,
              "telemetry": {                      # optional until real events exist: "n/d"
                                                  # window = robot route, SW_GAME_STATE_PLAYING
                "reads": 1200,                    # delta of sw_stats.reads over the window
                "read_misses": 7,                 # delta of sw_stats.read_misses
                "miss_start_events": 7,           # read_miss_start with t0 in the window; must
                                                  #   equal read_misses (else event loss)
                "core_misses": 0,                 # misses on `core` files (logic and collision)
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
required profile passed the automatic metrics; 1 for ``falhou`` or ``incompleto``; 2 for
unreadable input.
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
GATE_PROFILE = "cabo_50"  # the Gate 1 numbers (miss rate, core misses, wait p99) are decided here
MISS_RATE_LIMIT_PERCENT = 1.0  # docs/16: taxa de falta < 1% em cada rodada
WAIT_P99_LIMIT_MS = 300.0  # docs/16: espera numa falta, p99 < 300 ms
MIN_COMPLETE_ROUNDS = 3  # docs/16: pelo menos 3 rodadas completas e comparaveis
MIN_LOAD_RUNS = baseline.LOAD_MIN_RUNS
STUTTER_METRICS = tuple(baseline.LIMIT_MARGINS)
ENVIRONMENT_KEYS = ("machine", "gpu", "build", "route_sha256")
F1_48_NOTE = "mínimo pendente do F1-48"

ND = "n/d"
OK = "ok"
FAIL = "falhou"
INFO = "info"
PENDING = "incompleto"
UNFINISHED_KEYS = ("failed", "cancelled", "pending_at_end")
COUNT_KEYS = ("reads", "read_misses", "miss_start_events", *UNFINISHED_KEYS)


@dataclass
class Row:
    metric: str
    value: str
    criterion: str
    status: str  # ok | falhou | n/d | info | incompleto


@dataclass
class RoundDetail:
    """One run of one profile, as shown in the per-round table."""

    index: int
    reads: int | None = None
    misses: int | None = None
    core_misses: int | None = None
    waits_n: int | None = None
    p99_ms: float | None = None  # None = n/d (no samples), never 0
    complete: bool = False


@dataclass
class Aggregate:
    rounds: int  # complete rounds in the pool
    reads: int
    misses: int
    waits_n: int
    p99_ms: float | None


@dataclass
class ProfileResult:
    name: str
    rows: list[Row] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    details: list[RoundDetail] = field(default_factory=list)
    aggregate: Aggregate | None = None
    runs: int = 0
    verdict: str = PENDING


@dataclass
class RoundResult:
    round_id: str
    profiles: list[ProfileResult]
    verdict: str
    min_samples: int | None = None


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
    if "core_misses" in telemetry and not _is_count(telemetry["core_misses"]):
        reasons.append(f"{label}: telemetria.core_misses: esperado inteiro nao negativo")
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
    if telemetry.get("core_misses", 0) > misses:
        reasons.append(f"{label}: telemetria.core_misses > read_misses")
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


def _format_ms(value: float | None) -> str:
    return ND if value is None else f"{value:.2f}"


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


def _comparable(envs: Sequence[object]) -> bool:
    """Every run has all the environment keys and they are identical (docs/16, F1-48)."""
    mappings: list[Mapping[str, Any]] = [e for e in envs if isinstance(e, Mapping)]
    if not envs or len(mappings) != len(envs):
        return False
    complete = all(all(key in env for key in ENVIRONMENT_KEYS) for env in mappings)
    return complete and all(env == mappings[0] for env in mappings)


def _fill_details(
    result: ProfileResult, blocks: Sequence[Any]
) -> tuple[list[list[float] | None], list[int]]:
    """Per-run details; also the waits of each valid run (ms) and the unfinished counts."""
    waits_by_run: list[list[float] | None] = []
    for index, block in enumerate(blocks, start=1):
        problems = validate_telemetry(block, f"rodada {index}")
        detail = RoundDetail(index=index)
        result.details.append(detail)
        if problems or not isinstance(block, Mapping):
            result.notes.extend(problems)
            waits_by_run.append(None)
            continue
        waits = [float(w) / 1000.0 for w in block["miss_wait_us"]]
        waits_by_run.append(waits)
        detail.reads, detail.misses = block["reads"], block["read_misses"]
        detail.core_misses = block.get("core_misses")
        detail.waits_n = len(waits)
        detail.p99_ms = nearest_rank(waits, 0.99) if waits else None
        unfinished = sum(block[key] for key in UNFINISHED_KEYS)
        if unfinished:
            result.notes.append(
                f"rodada {index}: {unfinished} falta(s) não concluída(s) "
                "(failed/cancelled/pending_at_end): rodada incompleta (docs/08)"
            )
        detail.complete = not unfinished
    counts = [
        sum(b[key] for b in blocks if isinstance(b, Mapping) and not validate_telemetry(b, ""))
        for key in UNFINISHED_KEYS
    ]
    return waits_by_run, counts


def _rate_row(result: ProfileResult, is_gate: bool, all_valid: bool) -> Row:
    valid = [d for d in result.details if d.reads is not None]
    rates = [100.0 * (d.misses or 0) / (d.reads or 1) for d in valid]
    value = " · ".join(
        ND if d.reads is None else f"{100.0 * (d.misses or 0) / (d.reads or 1):.3f}%"
        for d in result.details
    )
    criterion = f"< {MISS_RATE_LIMIT_PERCENT:g}% em cada rodada" if is_gate else "informativo"
    if not valid:
        status = ND
    elif is_gate and any(rate >= MISS_RATE_LIMIT_PERCENT for rate in rates):
        status = FAIL
    elif not all_valid:
        status = PENDING
    else:
        status = OK if is_gate else INFO
    return Row("Taxa de falta, por rodada", value or ND, criterion, status)


def _core_row(result: ProfileResult, all_valid: bool) -> Row:
    valid = [d for d in result.details if d.reads is not None]
    cores = [d.core_misses for d in valid]
    known = [c for c in cores if c is not None]
    if any(c > 0 for c in known):
        status = FAIL
    elif not known:
        status = ND
    elif len(known) < len(result.details) or not all_valid:
        status = PENDING
    else:
        status = OK
    if valid and len(known) < len(valid):
        result.notes.append(
            "core_misses ausente em alguma rodada: faltas em lógica e colisão n/d "
            "(a contagem exata é parte do F1-48)"
        )
    value = f"{sum(known)} (total)" if known else ND
    return Row("Faltas em lógica e colisão (core)", value, "= 0 (lógica e colisão)", status)


def _wait_row(
    result: ProfileResult,
    is_gate: bool,
    waits_by_run: Sequence[list[float] | None],
    envs: Sequence[object],
    min_samples: int | None,
    all_valid: bool,
) -> Row:
    notes = result.notes
    pool = [
        i
        for i, d in enumerate(result.details)
        if d.reads is not None and d.complete and waits_by_run[i] is not None
    ]
    pool_waits = [w for i in pool for w in (waits_by_run[i] or [])]
    comparable = _comparable([envs[i] for i in pool]) if (is_gate and pool) else True
    p99 = nearest_rank(pool_waits, 0.99) if pool_waits else None
    result.aggregate = Aggregate(
        rounds=len(pool),
        reads=sum(result.details[i].reads or 0 for i in pool),
        misses=sum(result.details[i].misses or 0 for i in pool),
        waits_n=len(pool_waits),
        p99_ms=p99,
    )
    value = (
        f"{_format_ms(p99)} (N={len(pool_waits)}, {len(pool)} rodadas)"
        if pool_waits
        else f"n/d (N=0, {len(pool)} rodadas)"
    )
    criterion = (
        f"p99 < {WAIT_P99_LIMIT_MS:g} ms, eventos reunidos de >= {MIN_COMPLETE_ROUNDS} rodadas "
        "completas e comparáveis"
        if is_gate
        else "informativo"
    )
    row = Row("Espera numa falta, p99 agregado (ms)", value, criterion, PENDING)
    if not is_gate:
        row.status = INFO if pool_waits else ND
    elif p99 is None:
        row.status = ND
    elif (
        len(pool) >= MIN_COMPLETE_ROUNDS
        and comparable
        and min_samples is not None
        and len(pool_waits) >= min_samples
        and p99 >= WAIT_P99_LIMIT_MS
    ):
        row.status = FAIL  # a slow pool fails even while other data is still missing
    elif len(pool) < MIN_COMPLETE_ROUNDS:
        notes.append(
            f"p99 agregado: {len(pool)} rodada(s) completa(s); exige >= {MIN_COMPLETE_ROUNDS}"
        )
    elif not comparable:
        notes.append(
            "rodadas não comparáveis: environment ausente, sem "
            f"{'/'.join(ENVIRONMENT_KEYS)} ou diferente entre as rodadas "
            "(o que torna rodadas comparáveis é parte do F1-48)"
        )
    elif min_samples is None:
        notes.append(f"p99 agregado: {F1_48_NOTE} (informe --min-samples)")
    elif len(pool_waits) < min_samples:
        notes.append(f"p99 agregado: N={len(pool_waits)} abaixo do mínimo {min_samples}")
    elif all_valid and len(pool) == len(result.details):
        row.status = OK
    return row


def _telemetry_rows(
    name: str,
    profile: Mapping[str, Any],
    runs: Sequence[Any],
    result: ProfileResult,
    min_samples: int | None,
) -> list[Row]:
    is_gate = name == GATE_PROFILE
    blocks = [run.get("telemetry") if isinstance(run, Mapping) else None for run in runs]
    waits_by_run, counts = _fill_details(result, blocks)
    valid = [d for d in result.details if d.reads is not None]
    all_valid = len(valid) == len(result.details) and bool(valid)
    envs = [
        run.get("environment", profile.get("environment")) if isinstance(run, Mapping) else None
        for run in runs
    ]
    rows = [_rate_row(result, is_gate, all_valid)]
    if is_gate:
        rows.append(_core_row(result, all_valid))
    rows.append(_wait_row(result, is_gate, waits_by_run, envs, min_samples, all_valid))
    unfinished_status = ND if not valid else (PENDING if any(counts) or not all_valid else OK)
    rows.append(
        Row(
            "Faltas não concluídas (failed / cancelled / pending_at_end)",
            ND if not valid else " / ".join(str(c) for c in counts),
            "= 0",
            unfinished_status,
        )
    )
    return rows


def evaluate_profile(
    name: str, profile: object, limits: Mapping[str, Any], min_samples: int | None = None
) -> ProfileResult:
    result = ProfileResult(name=name)
    profile_map: Mapping[str, Any] = profile if isinstance(profile, Mapping) else {}
    runs = profile_map.get("runs")
    if not isinstance(runs, list) or not runs:
        result.notes.append("sem rodadas")
        result.rows = _stutter_rows([], limits, 0) + _telemetry_rows(
            name, profile_map, [], result, min_samples
        )
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
        name, profile_map, runs, result, min_samples
    )
    statuses = {row.status for row in result.rows}
    if FAIL in statuses:
        result.verdict = FAIL
    elif statuses <= {OK, INFO}:
        result.verdict = "passou"
    else:
        result.verdict = PENDING
    return result


def evaluate_round(
    report: Mapping[str, Any], limits: Mapping[str, Any], min_samples: int | None = None
) -> RoundResult:
    profiles = report.get("profiles")
    profiles = profiles if isinstance(profiles, Mapping) else {}
    names = list(REQUIRED_PROFILES) + sorted(n for n in profiles if n not in REQUIRED_PROFILES)
    results = []
    for name in names:
        if name in profiles:
            results.append(evaluate_profile(name, profiles[name], limits, min_samples))
        else:
            results.append(ProfileResult(name=name, notes=["perfil ausente"]))
    if any(p.verdict == FAIL for p in results):
        verdict = FAIL
    elif all(p.verdict == "passou" for p in results if p.name in REQUIRED_PROFILES):
        verdict = "passou"
    else:
        verdict = PENDING
    return RoundResult(
        round_id=str(report.get("round_id", "sem id")),
        profiles=results,
        verdict=verdict,
        min_samples=min_samples,
    )


def _round_table(profile: ProfileResult) -> list[str]:
    if not profile.details:
        return []
    lines = [
        "",
        "| Rodada | Leituras | Faltas | Taxa | Faltas core | Amostras (N) | p99 (ms) | Completa |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for d in profile.details:
        rate = ND if d.reads is None else f"{100.0 * (d.misses or 0) / (d.reads or 1):.3f}%"
        cells = [
            str(d.index),
            ND if d.reads is None else str(d.reads),
            ND if d.misses is None else str(d.misses),
            rate,
            ND if d.core_misses is None else str(d.core_misses),
            ND if d.waits_n is None else str(d.waits_n),
            _format_ms(d.p99_ms),
            "sim" if d.complete else "não",
        ]
        lines.append("| " + " | ".join(cells) + " |")
    agg = profile.aggregate
    if agg is not None:
        rate = f"{100.0 * agg.misses / agg.reads:.3f}%" if agg.reads else ND
        lines.append(
            f"| **Agregado** ({agg.rounds} completas) | {agg.reads} | {agg.misses} | {rate} | — | "
            f"{agg.waits_n} | {_format_ms(agg.p99_ms)} | — |"
        )
    return lines


def render_markdown(result: RoundResult) -> str:
    minimum = (
        f"mínimo de amostras para o p99 agregado: {result.min_samples}"
        if result.min_samples is not None
        else f"**{F1_48_NOTE}** (o p99 agregado fica `incompleto` até informar --min-samples)"
    )
    lines = [
        f"# Relatório da rodada {result.round_id}",
        "",
        f"**Métricas automáticas: {result.verdict}**",
        "",
        "**Métricas automáticas aprovadas NÃO aprovam sozinhas o Portão 1:** falta a avaliação "
        "visual registrada de pop-in (manual, fora deste relatório).",
        "",
        "Regra do Portão 1 (docs/16 e docs/08), decidida em `cabo_50`: taxa de falta < 1% em cada "
        "rodada; zero faltas em lógica e colisão; espera p99 < 300 ms sobre os eventos individuais "
        "reunidos de pelo menos 3 rodadas completas e comparáveis (nunca a média dos p99). "
        "Stutter e carga dentro dos limites absolutos aprovados em todos os perfis. "
        "`n/d` = métrica ausente, inválida ou sem amostras (nunca 0); a rodada fica `incompleto`, "
        "nunca `passou`.",
        "",
        f"Janela da telemetria: rota do robô com `SW_GAME_STATE_PLAYING`, carga inicial à parte. "
        f"{minimum}.",
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
        lines += _round_table(profile)
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
    parser.add_argument(
        "--min-samples",
        type=int,
        help="minimum pooled completed misses for the aggregate p99 (F1-48); without it the "
        "aggregate p99 is incompleto",
    )
    parser.add_argument("--out", type=Path, help="write the Markdown report here too")
    args = parser.parse_args(argv)
    if args.min_samples is not None and args.min_samples < 1:
        print("--min-samples: esperado inteiro >= 1", file=sys.stderr)
        return 2
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
    result = evaluate_round(report, limits, args.min_samples)
    text = render_markdown(result)
    sys.stdout.write(text)
    if args.out:
        args.out.write_text(text, encoding="utf-8", newline="\n")
    return 0 if result.verdict == "passou" else 1


if __name__ == "__main__":
    raise SystemExit(main())
