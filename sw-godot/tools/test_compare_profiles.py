# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Tests for F1-47 (known-change validation). Every number is a test fixture, not a measurement."""

from __future__ import annotations

import contextlib
import copy
import io
import json
import re
import tempfile
import unittest
from pathlib import Path
from typing import Any

import compare_profiles
import round_report

REPO = Path(__file__).resolve().parents[2]
ENV = {"machine": "m1", "gpu": "GTX 1050 Ti", "build": "abc123", "route_sha256": "ff" * 32}
MIN_SAMPLES = 20


def run(wait_ms: float, load_ms: float, misses: int = 10) -> dict[str, Any]:
    return {
        "load_ms": load_ms,
        "telemetry": {
            "reads": 1000,
            "read_misses": misses,
            "miss_start_events": misses,
            "miss_wait_us": [int(wait_ms * 1000 * (1 + i / 100)) for i in range(misses)],
            "failed": 0,
            "cancelled": 0,
            "pending_at_end": 0,
        },
    }


def profile(wait_ms: float, load_ms: float, runs: int = 3) -> dict[str, Any]:
    return {"environment": dict(ENV), "runs": [run(wait_ms, load_ms) for _ in range(runs)]}


def report(fast: dict[str, Any], slow: dict[str, Any]) -> dict[str, Any]:
    return {"schema": round_report.SCHEMA, "profiles": {"cabo_50": fast, "adsl_10": slow}}


def verdict(rep: dict[str, Any], min_samples: int | None = MIN_SAMPLES) -> str:
    return compare_profiles.compare(rep, min_samples=min_samples).verdict


class VerdictTests(unittest.TestCase):
    def test_slower_network_that_worsens_waits_and_load_is_coherent(self) -> None:
        rep = report(profile(190, 2500), profile(900, 4000))
        self.assertEqual(verdict(rep), "coerente")

    def test_only_the_waits_worsening_is_coherent(self) -> None:
        self.assertEqual(verdict(report(profile(190, 2500), profile(900, 2500))), "coerente")

    def test_only_the_load_worsening_is_coherent(self) -> None:
        self.assertEqual(verdict(report(profile(190, 2500), profile(190, 3500))), "coerente")

    def test_nothing_moving_means_the_bench_does_not_measure(self) -> None:
        """Risk R15: a slower profile that changes nothing."""
        self.assertEqual(verdict(report(profile(190, 2500), profile(190, 2500))), "nao_mede")

    def test_small_noise_below_the_minimums_is_not_a_measurement(self) -> None:
        self.assertEqual(verdict(report(profile(190, 2500), profile(250, 2600))), "nao_mede")

    def test_slow_profile_better_than_fast_is_incoherent(self) -> None:
        self.assertEqual(verdict(report(profile(900, 4000), profile(190, 2500))), "incoerente")

    def test_one_metric_worsens_and_another_improves_is_incoherent(self) -> None:
        self.assertEqual(verdict(report(profile(190, 4000), profile(900, 2500))), "incoerente")

    def test_control_with_the_same_profile_and_small_noise_is_not_incoherent(self) -> None:
        """A-A' control (protocol): 5% either way is noise, not a worse or better profile."""
        for factor in (0.95, 1.0, 1.05):
            with self.subTest(factor=factor):
                rep = report(profile(190, 2500), profile(190 * factor, 2500 * factor))
                self.assertEqual(verdict(rep), "nao_mede")

    def test_improvement_beyond_the_noise_is_incoherent(self) -> None:
        rep = report(profile(190, 2500), profile(190 * 0.8, 2500))
        self.assertEqual(verdict(rep), "incoerente")

    def test_missing_profile_is_incomplete(self) -> None:
        rep = report(profile(190, 2500), profile(900, 4000))
        del rep["profiles"]["adsl_10"]
        self.assertEqual(verdict(rep), "incompleto")
        self.assertEqual(verdict({}), "incompleto")

    def test_fewer_than_three_runs_is_incomplete(self) -> None:
        self.assertEqual(
            verdict(report(profile(190, 2500), profile(900, 4000, runs=2))), "incompleto"
        )

    def test_invalid_telemetry_or_load_is_incomplete(self) -> None:
        mutations: list[dict[str, Any]] = [
            {"telemetry": {"miss_start_events": 99}},  # event loss
            {"telemetry": {"failed": 1}},  # unfinished miss
            {"load_ms": float("nan")},
            {"load_ms": True},
            {"load_ms": -1},
            {"telemetry": None},  # telemetry removed
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                rep = report(profile(190, 2500), profile(900, 4000))
                first = rep["profiles"]["adsl_10"]["runs"][0]
                for key, value in mutation.items():
                    if value is None:
                        first.pop(key)
                    elif isinstance(value, dict):
                        first[key].update(value)
                    else:
                        first[key] = value
                self.assertEqual(verdict(rep), "incompleto")

    def test_environment_must_exist_and_be_identical(self) -> None:
        rep = report(profile(190, 2500), profile(900, 4000))
        rep["profiles"]["adsl_10"]["environment"]["gpu"] = "Intel HD 630"
        self.assertEqual(verdict(rep), "incompleto")
        rep = report(profile(190, 2500), profile(900, 4000))
        del rep["profiles"]["cabo_50"]["environment"]
        self.assertEqual(verdict(rep), "incompleto")
        rep = report(profile(190, 2500), profile(900, 4000))
        del rep["profiles"]["adsl_10"]["environment"]["route_sha256"]
        self.assertEqual(verdict(rep), "incompleto")

    def test_without_a_minimum_the_verdict_is_never_coherent(self) -> None:
        """The minimum of samples is pending F1-48: no silent default."""
        result = compare_profiles.compare(report(profile(190, 2500), profile(900, 4000)))
        self.assertEqual(result.verdict, "incompleto")
        self.assertTrue(any("F1-48" in note for note in result.notes))

    def test_fewer_samples_than_the_minimum_is_incomplete(self) -> None:
        rep = report(profile(190, 2500), profile(900, 4000))
        self.assertEqual(verdict(rep, 1000), "incompleto")

    def test_a_ratio_far_above_the_expected_one_is_flagged_not_failed(self) -> None:
        result = compare_profiles.compare(
            report(profile(100, 2500), profile(5000, 4000)), min_samples=MIN_SAMPLES
        )
        self.assertEqual(result.verdict, "coerente")
        self.assertTrue(any("investigar" in note for note in result.notes))

    def test_input_is_not_modified(self) -> None:
        rep = report(profile(190, 2500), profile(900, 4000))
        before = copy.deepcopy(rep)
        compare_profiles.compare(rep, min_samples=MIN_SAMPLES)
        self.assertEqual(rep, before)


class ExpectedRatioTests(unittest.TestCase):
    def test_network_constants_match_sw_netlab(self) -> None:
        """compare_profiles mirrors sw-netlab/server.py PROFILES; fail if they drift."""
        text = (REPO / "sw-netlab" / "server.py").read_text(encoding="utf-8")
        for name, (bandwidth, latency) in compare_profiles.NETWORK.items():
            found = re.search(rf'"{name}": NetworkProfile\(([\d.]+), ([\d.]+), [\d.]+\)', text)
            if found is None:
                self.fail(f"perfil {name} nao encontrado em sw-netlab/server.py")
            self.assertEqual((float(found.group(1)), float(found.group(2))), (bandwidth, latency))

    def test_expected_waits_for_one_mib_piece(self) -> None:
        self.assertAlmostEqual(compare_profiles.expected_wait_ms("cabo_50"), 192.77, places=1)
        self.assertAlmostEqual(compare_profiles.expected_wait_ms("adsl_10"), 898.86, places=1)
        result = compare_profiles.compare({})
        self.assertAlmostEqual(result.expected_ratio, 4.66, places=1)

    def test_ratio_helper_handles_zero(self) -> None:
        self.assertEqual(compare_profiles.ratio(4.0, 2.0), 2.0)
        self.assertEqual(compare_profiles.ratio(0.0, 0.0), 1.0)
        self.assertEqual(compare_profiles.ratio(5.0, 0.0), float("inf"))


class CliTests(unittest.TestCase):
    def run_cli(self, rep: object, *extra: str) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "round.json"
            path.write_text(json.dumps(rep), encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = compare_profiles.main([str(path), *extra])
            return code, out.getvalue(), err.getvalue()

    def test_exit_zero_only_for_coherent(self) -> None:
        code, out, _ = self.run_cli(
            report(profile(190, 2500), profile(900, 4000)), "--min-samples", "20"
        )
        self.assertEqual(code, 0)
        self.assertIn("**Veredito: coerente**", out)
        self.assertIn("Razão esperada", out)

    def test_exit_one_for_everything_else(self) -> None:
        cases = {
            "nao_mede": report(profile(190, 2500), profile(190, 2500)),
            "incoerente": report(profile(900, 4000), profile(190, 2500)),
            "incompleto": {},
        }
        for expected, rep in cases.items():
            with self.subTest(expected=expected):
                code, out, _ = self.run_cli(rep, "--min-samples", "20")
                self.assertEqual(code, 1)
                self.assertIn(f"**Veredito: {expected}**", out)

    def test_exit_two_for_unreadable_input(self) -> None:
        self.assertEqual(self.run_cli([1, 2])[0], 2)
        self.assertEqual(self.run_cli({"schema": "outro"})[0], 2)
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(compare_profiles.main(["does-not-exist.json"]), 2)

    def test_other_profile_pair_is_allowed(self) -> None:
        rep = {
            "profiles": {
                "fibra_300": profile(60, 2500),
                "cabo_50": profile(190, 3000),
            }
        }
        code, out, _ = self.run_cli(
            rep, "--fast", "fibra_300", "--slow", "cabo_50", "--min-samples", "20"
        )
        self.assertEqual(code, 0, out)


if __name__ == "__main__":
    unittest.main()
