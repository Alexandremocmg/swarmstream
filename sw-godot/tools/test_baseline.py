# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Regression tests for F1-36. Run beside baseline.py with unittest."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import baseline


class BaselineCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.limits = {
            "frames_over_33ms_steady": 2.0,
            "frames_over_100ms": 0.0,
            "p99_frame_ms": 6.238,
            "load_ms": 3059.0544,
        }
        self.report = {
            "frames_over_33ms_steady": 1,
            "frames_over_100ms": 0,
            "p99_frame_ms": 4.238,
            "load_ms": 2549.515,
        }

    def check_cli(self, reports: list[object]) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            limits_path = directory / "limits.json"
            limits_path.write_text(json.dumps(self.limits), encoding="utf-8")
            paths = []
            for index, report in enumerate(reports):
                path = directory / f"run-{index}.json"
                path.write_text(json.dumps(report), encoding="utf-8")
                paths.append(str(path))
            return subprocess.run(
                [
                    sys.executable,
                    str(Path(baseline.__file__).resolve()),
                    "--godot",
                    "unused-in-check-mode",
                    "--check",
                    *paths,
                    "--limits",
                    str(limits_path),
                ],
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )

    def test_cli_rejects_three_empty_reports(self) -> None:
        result = self.check_cli([{}, {}, {}])
        self.assertEqual(result.returncode, 1, result.stderr)
        for index in range(1, 4):
            for name in baseline.REQUIRED_CHECK_METRICS:
                self.assertIn(f"rodada {index}: metrica obrigatoria ausente: {name}", result.stdout)
        self.assertNotIn("within limits", result.stdout)

    def test_cli_rejects_three_reports_with_wrong_keys(self) -> None:
        wrong = {f"wrong_{name}": value for name, value in self.report.items()}
        result = self.check_cli([wrong, wrong, wrong])
        self.assertEqual(result.returncode, 1, result.stderr)
        for name in baseline.REQUIRED_CHECK_METRICS:
            self.assertIn(name, result.stdout)
        self.assertNotIn("within limits", result.stdout)

    def test_each_required_metric_must_exist_in_every_run(self) -> None:
        for name in baseline.REQUIRED_CHECK_METRICS:
            with self.subTest(metric=name):
                partial = self.report.copy()
                del partial[name]
                result = self.check_cli([self.report, partial, self.report])
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f"rodada 2: metrica obrigatoria ausente: {name}", result.stdout)

    def test_cli_accepts_valid_reports(self) -> None:
        result = self.check_cli([self.report] * 3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "within limits")

    def test_recorded_gpu_runs_pass_approved_limits(self) -> None:
        local = Path(__file__).resolve().parent
        repo_data = local.parents[1] / "docs" / "medicoes"
        data = local if (local / "baseline-gpu.json").is_file() else repo_data
        summary = json.loads((data / "baseline-gpu.json").read_text(encoding="utf-8"))
        approved = json.loads((data / "baseline-limits.json").read_text(encoding="utf-8"))
        self.assertEqual(baseline.check_against_limits(summary["runs"], approved), [])

    def test_invalid_numbers_are_rejected(self) -> None:
        for name in baseline.REQUIRED_CHECK_METRICS:
            for value in (None, True, "0", -1, float("nan"), float("inf")):
                with self.subTest(metric=name, value=value):
                    report = {**self.report, name: value}
                    failures = baseline.check_against_limits([report] * 3, self.limits)
                    self.assertTrue(any(name in failure for failure in failures))

    def test_empty_or_incomplete_limits_are_rejected(self) -> None:
        for missing in baseline.REQUIRED_CHECK_METRICS:
            with self.subTest(metric=missing):
                limits = self.limits.copy()
                del limits[missing]
                failures = baseline.check_against_limits([self.report] * 3, limits)
                self.assertIn(f"limites: metrica obrigatoria ausente: {missing}", failures)
        self.assertTrue(baseline.check_against_limits([self.report] * 3, {}))

    def test_empty_run_list_is_rejected(self) -> None:
        self.assertIn("nenhuma rodada informada", baseline.check_against_limits([], self.limits))

    def test_at_least_three_load_measurements_are_required(self) -> None:
        failures = baseline.check_against_limits([self.report] * 2, self.limits)
        self.assertTrue(any("pelo menos 3" in failure for failure in failures))

    def test_exact_limits_are_accepted(self) -> None:
        self.assertEqual(baseline.check_against_limits([self.limits] * 3, self.limits), [])

    def test_each_stutter_regression_is_rejected(self) -> None:
        for name in baseline.LIMIT_MARGINS:
            with self.subTest(metric=name):
                report = {**self.report, name: self.limits[name] + 1}
                result = self.check_cli([report, self.report, self.report])
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(name, result.stdout)

    def test_load_uses_median_not_worst_run(self) -> None:
        slow = {**self.report, "load_ms": self.limits["load_ms"] + 1000}
        self.assertEqual(
            baseline.check_against_limits([slow, self.report, self.report], self.limits), []
        )
        failures = baseline.check_against_limits([slow, slow, self.report], self.limits)
        self.assertTrue(any("load_ms: mediana" in failure for failure in failures))

    def test_summary_file_is_not_a_single_run(self) -> None:
        result = self.check_cli([{"runs": [self.report] * 3}] * 3)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("metrica obrigatoria ausente", result.stdout)

    def test_wrong_json_shapes_are_rejected(self) -> None:
        result = self.check_cli([None, [], "invalid"])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("esperado objeto JSON", result.stdout)


if __name__ == "__main__":
    unittest.main()
