# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Tests for F1-33 (round report, Gate 1 rule of 2026-10-19). Numbers are test fixtures."""

from __future__ import annotations

import contextlib
import io
import itertools
import json
import tempfile
import unittest
from pathlib import Path
from typing import Any

import baseline
import round_report

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
LIMITS = {
    "frames_over_33ms_steady": 2.0,
    "frames_over_100ms": 0.0,
    "p99_frame_ms": 6.238,
    "load_ms": 3059.0544,
}
ENV = {"machine": "m1", "gpu": "GTX 1050 Ti", "build": "abc123", "route_sha256": "ff" * 32}
MIN_SAMPLES = 10


def telemetry(
    waits_ms: list[float], reads: int = 1000, core_misses: int | None = 0, **counts: int
) -> dict[str, Any]:
    failed = counts.get("failed", 0)
    cancelled = counts.get("cancelled", 0)
    pending = counts.get("pending_at_end", 0)
    misses = len(waits_ms) + failed + cancelled + pending
    block: dict[str, Any] = {
        "reads": reads,
        "read_misses": misses,
        "miss_start_events": misses,
        "miss_wait_us": [int(w * 1000) for w in waits_ms],
        "failed": failed,
        "cancelled": cancelled,
        "pending_at_end": pending,
    }
    if core_misses is not None:
        block["core_misses"] = core_misses
    return block


def good_run(waits_ms: list[float] | None = None) -> dict[str, Any]:
    return {
        "load_ms": 2500.0,
        "frames_over_33ms_steady": 1,
        "frames_over_100ms": 0,
        "p99_frame_ms": 4.0,
        "telemetry": telemetry(
            waits_ms if waits_ms is not None else [10.0 * (i + 1) for i in range(5)]
        ),
    }


def good_profile() -> dict[str, Any]:
    return {"environment": dict(ENV), "runs": [good_run(), good_run(), good_run()]}


def full_round() -> dict[str, Any]:
    return {
        "schema": round_report.SCHEMA,
        "profiles": {name: good_profile() for name in round_report.REQUIRED_PROFILES},
    }


def cabo(report: dict[str, Any]) -> dict[str, Any]:
    profile: dict[str, Any] = report["profiles"]["cabo_50"]
    return profile


def evaluate(
    report: dict[str, Any], min_samples: int | None = MIN_SAMPLES
) -> round_report.RoundResult:
    return round_report.evaluate_round(report, LIMITS, min_samples)


def verdicts(report: dict[str, Any], min_samples: int | None = MIN_SAMPLES) -> dict[str, str]:
    return {p.name: p.verdict for p in evaluate(report, min_samples).profiles}


def profile_result(report: dict[str, Any], name: str = "cabo_50") -> round_report.ProfileResult:
    return next(p for p in evaluate(report).profiles if p.name == name)


class GateRuleTests(unittest.TestCase):
    def test_complete_good_round_passes_the_automatic_metrics(self) -> None:
        result = evaluate(full_round())
        self.assertEqual(result.verdict, "passou")
        self.assertEqual(set(verdicts(full_round()).values()), {"passou"})

    def test_empty_round_is_incomplete_never_passed(self) -> None:
        empties: list[dict[str, Any]] = [{}, {"schema": round_report.SCHEMA}, {"profiles": {}}]
        for empty in empties:
            with self.subTest(report=empty):
                result = evaluate(empty)
                self.assertEqual(result.verdict, "incompleto")
                self.assertEqual({p.verdict for p in result.profiles}, {"incompleto"})

    def test_missing_profile_is_incomplete(self) -> None:
        report = full_round()
        del report["profiles"]["adsl_10"]
        self.assertEqual(verdicts(report)["adsl_10"], "incompleto")
        self.assertEqual(evaluate(report).verdict, "incompleto")

    def test_miss_rate_is_checked_in_every_round_not_on_the_average(self) -> None:
        report = full_round()
        runs = cabo(report)["runs"]
        runs[0]["telemetry"] = telemetry([10.0] * 15)  # 1.5% in this round; the others 0.5%
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")
        rate_row = next(r for r in profile_result(report).rows if "por rodada" in r.metric)
        self.assertEqual(rate_row.status, "falhou")

    def test_exactly_one_percent_in_a_round_fails(self) -> None:
        report = full_round()
        cabo(report)["runs"][1]["telemetry"] = telemetry([10.0] * 10)
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_any_miss_in_logic_and_collision_fails(self) -> None:
        report = full_round()
        cabo(report)["runs"][2]["telemetry"] = telemetry([10.0] * 5, core_misses=1)
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_core_misses_missing_or_invalid_is_incomplete(self) -> None:
        report = full_round()
        cabo(report)["runs"][0]["telemetry"] = telemetry([10.0] * 5, core_misses=None)
        self.assertEqual(verdicts(report)["cabo_50"], "incompleto")
        report = full_round()
        cabo(report)["runs"][0]["telemetry"]["core_misses"] = 6  # more than read_misses (5)
        self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_p99_uses_the_pooled_individual_events_not_the_mean_of_p99(self) -> None:
        """Two rounds without outliers plus a tiny one with two slow misses: the mean of the three
        round p99s is far below 300 ms, but the pooled events have 2 of 102 above it."""
        report = full_round()
        runs = cabo(report)["runs"]
        runs[0]["telemetry"] = telemetry([10.0] * 50, reads=100_000)
        runs[1]["telemetry"] = telemetry([10.0] * 50, reads=100_000)
        runs[2]["telemetry"] = telemetry([400.0, 400.0], reads=100_000)
        details = profile_result(report).details
        self.assertLess(sum(d.p99_ms or 0.0 for d in details) / len(details), 300.0)
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_a_single_round_p99_above_the_limit_does_not_fail_when_the_pool_is_fine(self) -> None:
        report = full_round()
        runs = cabo(report)["runs"]
        runs[0]["telemetry"] = telemetry([10.0] * 4 + [400.0], reads=100_000)
        for run in runs[1:]:
            run["telemetry"] = telemetry([10.0] * 200, reads=100_000)
        result = profile_result(report)
        self.assertGreaterEqual(result.details[0].p99_ms or 0.0, 300.0)
        self.assertEqual(result.verdict, "passou")

    def test_p99_at_or_above_300_ms_fails(self) -> None:
        report = full_round()
        for run in cabo(report)["runs"]:
            run["telemetry"] = telemetry([10.0] * 4 + [300.0])
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_no_misses_is_nd_not_zero(self) -> None:
        report = full_round()
        for run in cabo(report)["runs"]:
            run["telemetry"] = telemetry([])
        result = profile_result(report)
        self.assertEqual(result.verdict, "incompleto")
        self.assertTrue(all(d.p99_ms is None for d in result.details))
        text = round_report.render_markdown(evaluate(report))
        self.assertIn("n/d", text)
        self.assertNotIn("| 0.00 |", text)

    def test_one_round_without_misses_has_nd_p99_but_the_pool_still_counts(self) -> None:
        report = full_round()
        cabo(report)["runs"][0]["telemetry"] = telemetry([])
        result = profile_result(report)
        self.assertIsNone(result.details[0].p99_ms)
        self.assertEqual(result.verdict, "passou")  # 10 pooled events >= MIN_SAMPLES

    def test_pooled_samples_below_the_minimum_are_incomplete(self) -> None:
        self.assertEqual(verdicts(full_round(), min_samples=10_000)["cabo_50"], "incompleto")

    def test_minimum_pending_f1_48_is_never_a_pass(self) -> None:
        result = evaluate(full_round(), min_samples=None)
        cabo_result = next(p for p in result.profiles if p.name == "cabo_50")
        self.assertEqual(cabo_result.verdict, "incompleto")
        self.assertEqual(result.verdict, "incompleto")
        self.assertTrue(any("F1-48" in note for note in cabo_result.notes))
        self.assertIn("mínimo pendente do F1-48", round_report.render_markdown(result))

    def test_fewer_than_three_complete_rounds_is_incomplete(self) -> None:
        report = full_round()
        cabo(report)["runs"] = cabo(report)["runs"][:2]
        self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_an_unfinished_miss_makes_that_round_incomplete_and_leaves_the_pool(self) -> None:
        for outcome in ("failed", "cancelled", "pending_at_end"):
            with self.subTest(outcome=outcome):
                report = full_round()
                cabo(report)["runs"][0]["telemetry"] = telemetry([10.0] * 4, **{outcome: 1})
                result = profile_result(report)
                self.assertEqual(result.verdict, "incompleto")
                self.assertFalse(result.details[0].complete)
                self.assertTrue(all(d.complete for d in result.details[1:]))

    def test_rounds_must_be_comparable(self) -> None:
        report = full_round()
        cabo(report)["runs"][1]["environment"] = {**ENV, "gpu": "Intel HD 630"}
        self.assertEqual(verdicts(report)["cabo_50"], "incompleto")
        report = full_round()
        del cabo(report)["environment"]
        self.assertEqual(verdicts(report)["cabo_50"], "incompleto")
        report = full_round()
        del cabo(report)["environment"]["route_sha256"]
        self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_environment_can_be_given_per_run(self) -> None:
        report = full_round()
        profile = cabo(report)
        for run in profile["runs"]:
            run["environment"] = dict(profile["environment"])
        del profile["environment"]
        self.assertEqual(verdicts(report)["cabo_50"], "passou")

    def test_event_loss_is_incomplete(self) -> None:
        for events in (4, 6):
            with self.subTest(events=events):
                report = full_round()
                cabo(report)["runs"][1]["telemetry"]["miss_start_events"] = events
                result = profile_result(report)
                self.assertEqual(result.verdict, "incompleto")
                self.assertTrue(any("perda de evento" in note for note in result.notes))

    def test_invalid_telemetry_is_incomplete(self) -> None:
        base = good_run()["telemetry"]
        bad_values: list[dict[str, Any]] = [
            {**base, "reads": 0, "read_misses": 0, "miss_start_events": 0, "miss_wait_us": []},
            {**base, "reads": 3, "read_misses": 5, "miss_start_events": 5},
            {**base, "miss_wait_us": [float("nan")] * 5},
            {**base, "miss_wait_us": [-5] * 5},
            {**base, "miss_wait_us": [1] * 4},
            {**base, "reads": True},
            {**base, "failed": -1},
            {k: v for k, v in base.items() if k != "pending_at_end"},
            {k: v for k, v in base.items() if k != "miss_start_events"},
        ]
        for block in bad_values:
            with self.subTest(block=block):
                report = full_round()
                cabo(report)["runs"][0]["telemetry"] = block
                self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_failure_wins_over_missing_data(self) -> None:
        report = full_round()
        runs = cabo(report)["runs"]
        del runs[0]["telemetry"]
        runs[1]["telemetry"] = telemetry([10.0] * 15)  # 1.5%
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_missing_telemetry_shows_nd_and_is_incomplete(self) -> None:
        report = full_round()
        for run in cabo(report)["runs"]:
            del run["telemetry"]
        result = evaluate(report)
        self.assertEqual(
            next(p for p in result.profiles if p.name == "cabo_50").verdict, "incompleto"
        )
        self.assertIn("n/d", round_report.render_markdown(result))
        self.assertNotEqual(result.verdict, "passou")

    def test_other_profiles_are_reported_but_do_not_decide_the_rate_or_the_wait(self) -> None:
        report = full_round()
        for run in report["profiles"]["adsl_10"]["runs"]:
            run["telemetry"] = telemetry([2000.0] * 40, reads=1000, core_misses=5)
        self.assertEqual(verdicts(report)["adsl_10"], "passou")
        self.assertEqual(verdicts(report)["cabo_50"], "passou")

    def test_the_removed_aggregation_choice_is_gone(self) -> None:
        text = round_report.render_markdown(evaluate(full_round()))
        self.assertNotIn("pendente do dono", text)
        self.assertNotIn("rodadas juntas", text)


class StutterAndLoadTests(unittest.TestCase):
    """The approved stutter and load criteria are unchanged (docs/medicoes/baseline.md)."""

    def test_missing_stutter_metric_is_incomplete(self) -> None:
        for name in baseline.REQUIRED_CHECK_METRICS:
            with self.subTest(metric=name):
                report = full_round()
                del report["profiles"]["fibra_300"]["runs"][1][name]
                self.assertEqual(verdicts(report)["fibra_300"], "incompleto")

    def test_invalid_numbers_are_incomplete(self) -> None:
        for value in (None, True, "1", -1, float("nan"), float("inf")):
            with self.subTest(value=value):
                report = full_round()
                cabo(report)["runs"][0]["p99_frame_ms"] = value
                self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_fewer_than_three_runs_is_incomplete(self) -> None:
        report = full_round()
        report["profiles"]["fibra_300"]["runs"] = report["profiles"]["fibra_300"]["runs"][:2]
        self.assertEqual(verdicts(report)["fibra_300"], "incompleto")

    def test_stutter_regression_fails_on_every_profile(self) -> None:
        for profile in round_report.REQUIRED_PROFILES:
            for name in ("frames_over_33ms_steady", "frames_over_100ms", "p99_frame_ms"):
                with self.subTest(profile=profile, metric=name):
                    report = full_round()
                    report["profiles"][profile]["runs"][2][name] = LIMITS[name] + 1
                    self.assertEqual(verdicts(report)[profile], "falhou")

    def test_load_uses_the_median_of_runs(self) -> None:
        report = full_round()
        cabo(report)["runs"][0]["load_ms"] = LIMITS["load_ms"] + 5000
        self.assertEqual(verdicts(report)["cabo_50"], "passou")
        cabo(report)["runs"][1]["load_ms"] = LIMITS["load_ms"] + 5000
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_agrees_with_baseline_check_on_stutter_and_load(self) -> None:
        """The report must never accept what baseline.py --check rejects."""
        variants = [  # (steady > 33 ms, > 100 ms, p99 ms, load ms): at, over and under each limit
            (0, 0, 3.0, 2000.0),
            (2, 0, 6.238, 3059.0544),
            (3, 0, 4.0, 2500.0),
            (1, 1, 4.0, 2500.0),
            (1, 0, 6.3, 2500.0),
            (1, 0, 4.0, 3100.0),
        ]
        for count in (2, 3):  # 252 combinations; 4 runs add 1296 and no new case
            for combo in itertools.product(variants, repeat=count):
                runs = []
                for steady, over_100, p99, load in combo:
                    run = good_run()
                    run.update(
                        frames_over_33ms_steady=steady,
                        frames_over_100ms=over_100,
                        p99_frame_ms=p99,
                        load_ms=load,
                    )
                    runs.append(run)
                profile = good_profile()
                profile["runs"] = runs
                ours = round_report.evaluate_profile(
                    "cabo_50", profile, LIMITS, MIN_SAMPLES
                ).verdict
                theirs = baseline.check_against_limits(runs, LIMITS)
                self.assertEqual(ours == "passou", not theirs, (combo, ours, theirs))


class PercentileTests(unittest.TestCase):
    def test_nearest_rank(self) -> None:
        values = list(range(1, 101))
        self.assertEqual(round_report.nearest_rank(values, 0.50), 50)
        self.assertEqual(round_report.nearest_rank(values, 0.95), 95)
        self.assertEqual(round_report.nearest_rank(values, 0.99), 99)
        self.assertEqual(round_report.nearest_rank([7], 0.99), 7)
        self.assertEqual(round_report.nearest_rank([3, 1, 2], 0.99), 3)

    def test_below_100_samples_p99_is_the_maximum(self) -> None:
        self.assertEqual(round_report.nearest_rank(list(range(1, 51)), 0.99), 50)


class RenderTests(unittest.TestCase):
    def render(self, report: dict[str, Any], min_samples: int | None = MIN_SAMPLES) -> str:
        return round_report.render_markdown(evaluate(report, min_samples))

    def test_shows_individual_and_aggregate_results_with_sample_counts(self) -> None:
        text = self.render(full_round())
        for header in ("Leituras", "Faltas", "Amostras (N)", "p99 (ms)"):
            self.assertIn(header, text)
        self.assertIn("Agregado", text)
        self.assertIn("N=15", text)  # 3 rounds x 5 misses pooled

    def test_markdown_has_all_profiles_and_never_hides_incomplete(self) -> None:
        text = self.render({})
        for name in round_report.REQUIRED_PROFILES:
            self.assertIn(name, text)
        self.assertIn("**Métricas automáticas: incompleto**", text)

    def test_automatic_metrics_never_approve_gate_1_alone(self) -> None:
        text = self.render(full_round())
        self.assertIn("**Métricas automáticas: passou**", text)
        self.assertIn("NÃO aprovam sozinhas o Portão 1", text)
        self.assertIn("avaliação visual registrada de pop-in", text)
        self.assertNotIn("Portão 1: passou", text)

    def test_example_output_is_up_to_date(self) -> None:
        example = HERE / "examples" / "round_baseline_gpu.json"
        expected = (HERE / "examples" / "round_baseline_gpu.md").read_text(encoding="utf-8")
        limits = json.loads(
            (REPO / "docs" / "medicoes" / "baseline-limits.json").read_text("utf-8")
        )
        report = json.loads(example.read_text(encoding="utf-8"))
        got = round_report.render_markdown(round_report.evaluate_round(report, limits, None))
        self.assertEqual(got, expected)


class CliTests(unittest.TestCase):
    def run_cli(self, report: object, *extra: str, limits: object = LIMITS) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            round_path = directory / "round.json"
            limits_path = directory / "limits.json"
            round_path.write_text(json.dumps(report), encoding="utf-8")
            limits_path.write_text(json.dumps(limits), encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = round_report.main([str(round_path), "--limits", str(limits_path), *extra])
            return code, out.getvalue(), err.getvalue()

    def test_exit_zero_only_when_everything_passed_and_a_minimum_was_given(self) -> None:
        code, out, _ = self.run_cli(full_round(), "--min-samples", str(MIN_SAMPLES))
        self.assertEqual(code, 0)
        self.assertIn("**Métricas automáticas: passou**", out)
        code, out, _ = self.run_cli(full_round())
        self.assertEqual(code, 1)
        self.assertIn("mínimo pendente do F1-48", out)

    def test_exit_one_for_incomplete_and_for_failed(self) -> None:
        code, out, _ = self.run_cli({}, "--min-samples", "10")
        self.assertEqual(code, 1)
        self.assertIn("**Métricas automáticas: incompleto**", out)
        failed = full_round()
        cabo(failed)["runs"][0]["frames_over_100ms"] = 9
        self.assertEqual(self.run_cli(failed, "--min-samples", "10")[0], 1)

    def test_exit_two_for_unreadable_input(self) -> None:
        code, _, err = self.run_cli([1, 2, 3])
        self.assertEqual(code, 2)
        self.assertIn("objeto", err)
        code, _, err = self.run_cli({"schema": "outro"})
        self.assertEqual(code, 2)
        self.assertIn("schema", err)
        code, _, err = self.run_cli(full_round(), limits={})
        self.assertEqual(code, 2)
        self.assertIn("limites", err)
        code, _, err = self.run_cli(full_round(), "--min-samples", "0")
        self.assertEqual(code, 2)
        self.assertIn("min-samples", err)

    def test_missing_file_is_exit_two(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = round_report.main(["does-not-exist.json", "--limits", "nope.json"])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
