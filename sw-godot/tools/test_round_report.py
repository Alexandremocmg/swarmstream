# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Tests for F1-33 (round report skeleton). Run beside round_report.py with unittest."""

from __future__ import annotations

import contextlib
import copy
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


def good_run() -> dict[str, Any]:
    return {
        "load_ms": 2500.0,
        "frames_over_33ms_steady": 1,
        "frames_over_100ms": 0,
        "p99_frame_ms": 4.0,
        "telemetry": {
            "reads": 1000,
            "read_misses": 5,
            "miss_wait_us": [10_000 * (i + 1) for i in range(5)],
            "unfinished_misses": 0,
        },
    }


def good_profile() -> dict[str, Any]:
    return {"runs": [good_run(), good_run(), good_run()]}


def full_round() -> dict[str, Any]:
    return {
        "schema": round_report.SCHEMA,
        "profiles": {name: good_profile() for name in round_report.REQUIRED_PROFILES},
    }


def verdicts(report: dict[str, Any]) -> dict[str, str]:
    result = round_report.evaluate_round(report, LIMITS)
    return {p.name: p.verdict for p in result.profiles}


class VerdictTests(unittest.TestCase):
    def test_complete_good_round_passes(self) -> None:
        result = round_report.evaluate_round(full_round(), LIMITS)
        self.assertEqual(result.verdict, "passou")
        self.assertEqual(set(verdicts(full_round()).values()), {"passou"})

    def test_empty_round_is_incomplete_never_passed(self) -> None:
        for empty in (
            {},
            {"schema": round_report.SCHEMA},
            {"schema": round_report.SCHEMA, "profiles": {}},
        ):
            with self.subTest(report=empty):
                result = round_report.evaluate_round(empty, LIMITS)
                self.assertEqual(result.verdict, "incompleto")
                self.assertEqual({p.verdict for p in result.profiles}, {"incompleto"})
                self.assertEqual(
                    [p.name for p in result.profiles], list(round_report.REQUIRED_PROFILES)
                )

    def test_missing_profile_is_incomplete(self) -> None:
        report = full_round()
        del report["profiles"]["adsl_10"]
        self.assertEqual(verdicts(report)["adsl_10"], "incompleto")
        self.assertEqual(round_report.evaluate_round(report, LIMITS).verdict, "incompleto")

    def test_missing_telemetry_shows_nd_and_is_incomplete(self) -> None:
        report = full_round()
        for run in report["profiles"]["cabo_50"]["runs"]:
            del run["telemetry"]
        result = round_report.evaluate_round(report, LIMITS)
        cabo = next(p for p in result.profiles if p.name == "cabo_50")
        self.assertEqual(cabo.verdict, "incompleto")
        self.assertIn("n/d", round_report.render_markdown(result))
        self.assertNotEqual(result.verdict, "passou")

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
                report["profiles"]["cabo_50"]["runs"][0]["p99_frame_ms"] = value
                self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_invalid_telemetry_is_incomplete(self) -> None:
        bad_values: list[dict[str, Any]] = [
            {"reads": 0, "read_misses": 0, "miss_wait_us": [], "unfinished_misses": 0},
            {"reads": 10, "read_misses": 11, "miss_wait_us": [], "unfinished_misses": 0},
            {"reads": 10, "read_misses": 1, "miss_wait_us": [float("nan")], "unfinished_misses": 0},
            {"reads": 10, "read_misses": 1, "miss_wait_us": [-5], "unfinished_misses": 0},
            {"reads": 10, "read_misses": 2, "miss_wait_us": [1], "unfinished_misses": 0},
            {"reads": 10, "read_misses": 1, "miss_wait_us": [1]},
            {"reads": True, "read_misses": 1, "miss_wait_us": [1], "unfinished_misses": 0},
        ]
        for telemetry in bad_values:
            with self.subTest(telemetry=telemetry):
                report = full_round()
                report["profiles"]["cabo_50"]["runs"][0]["telemetry"] = telemetry
                self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_unfinished_miss_makes_round_incomplete_even_if_p99_is_fine(self) -> None:
        report = full_round()
        report["profiles"]["cabo_50"]["runs"][0]["telemetry"]["unfinished_misses"] = 1
        self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_fewer_than_three_runs_is_incomplete(self) -> None:
        report = full_round()
        report["profiles"]["cabo_50"]["runs"] = report["profiles"]["cabo_50"]["runs"][:2]
        self.assertEqual(verdicts(report)["cabo_50"], "incompleto")

    def test_miss_rate_at_or_above_one_percent_fails_on_gate_profile(self) -> None:
        report = full_round()
        for run in report["profiles"]["cabo_50"]["runs"]:
            run["telemetry"]["read_misses"] = 10
            run["telemetry"]["miss_wait_us"] = [1_000] * 10
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_p99_wait_at_or_above_300_ms_fails_on_gate_profile(self) -> None:
        report = full_round()
        report["profiles"]["cabo_50"]["runs"][0]["telemetry"]["miss_wait_us"][-1] = 300_000
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_wait_and_miss_rate_are_informative_on_other_profiles(self) -> None:
        report = full_round()
        for run in report["profiles"]["adsl_10"]["runs"]:
            run["telemetry"]["miss_wait_us"] = [2_000_000] * 5
        self.assertEqual(verdicts(report)["adsl_10"], "passou")

    def test_stutter_regression_fails_on_every_profile(self) -> None:
        for profile in round_report.REQUIRED_PROFILES:
            for name in ("frames_over_33ms_steady", "frames_over_100ms", "p99_frame_ms"):
                with self.subTest(profile=profile, metric=name):
                    report = full_round()
                    report["profiles"][profile]["runs"][2][name] = LIMITS[name] + 1
                    self.assertEqual(verdicts(report)[profile], "falhou")

    def test_load_uses_median_of_runs(self) -> None:
        report = full_round()
        report["profiles"]["cabo_50"]["runs"][0]["load_ms"] = LIMITS["load_ms"] + 5000
        self.assertEqual(verdicts(report)["cabo_50"], "passou")
        report["profiles"]["cabo_50"]["runs"][1]["load_ms"] = LIMITS["load_ms"] + 5000
        self.assertEqual(verdicts(report)["cabo_50"], "falhou")

    def test_failure_wins_over_missing_data(self) -> None:
        report = full_round()
        del report["profiles"]["cabo_50"]["runs"][0]["telemetry"]
        report["profiles"]["cabo_50"]["runs"][1]["frames_over_100ms"] = 3
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
        for count in (2, 3, 4):
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
                report = full_round()
                report["profiles"]["cabo_50"]["runs"] = runs
                ours = verdicts(report)["cabo_50"]
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
    def test_markdown_has_all_profiles_and_never_hides_incomplete(self) -> None:
        text = round_report.render_markdown(round_report.evaluate_round({}, LIMITS))
        for name in round_report.REQUIRED_PROFILES:
            self.assertIn(name, text)
        self.assertIn("**Portão 1 (automático): incompleto**", text)

    def test_pop_in_is_declared_manual(self) -> None:
        text = round_report.render_markdown(round_report.evaluate_round(full_round(), LIMITS))
        self.assertIn("Pop-in", text)
        self.assertIn("manual", text)

    def test_example_output_is_up_to_date(self) -> None:
        example = HERE / "examples" / "round_baseline_gpu.json"
        expected = (HERE / "examples" / "round_baseline_gpu.md").read_text(encoding="utf-8")
        limits = json.loads(
            (REPO / "docs" / "medicoes" / "baseline-limits.json").read_text("utf-8")
        )
        report = json.loads(example.read_text(encoding="utf-8"))
        got = round_report.render_markdown(round_report.evaluate_round(report, limits))
        self.assertEqual(got, expected)


class CliTests(unittest.TestCase):
    def run_cli(self, report: object, limits: object = LIMITS) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            round_path = directory / "round.json"
            limits_path = directory / "limits.json"
            round_path.write_text(json.dumps(report), encoding="utf-8")
            limits_path.write_text(json.dumps(limits), encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = round_report.main([str(round_path), "--limits", str(limits_path)])
            return code, out.getvalue(), err.getvalue()

    def test_exit_zero_only_when_everything_passed(self) -> None:
        code, out, _ = self.run_cli(full_round())
        self.assertEqual(code, 0)
        self.assertIn("**Portão 1 (automático): passou**", out)

    def test_exit_one_for_incomplete_and_for_failed(self) -> None:
        code, out, _ = self.run_cli({})
        self.assertEqual(code, 1)
        self.assertIn("**Portão 1 (automático): incompleto**", out)
        failed = copy.deepcopy(full_round())
        failed["profiles"]["cabo_50"]["runs"][0]["frames_over_100ms"] = 9
        self.assertEqual(self.run_cli(failed)[0], 1)

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

    def test_missing_file_is_exit_two(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = round_report.main(["does-not-exist.json", "--limits", "nope.json"])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
