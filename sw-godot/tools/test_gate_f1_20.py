# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Testes do run() do portão F1-20: o que o processo escreveu antes do timeout não pode sumir."""

from __future__ import annotations

import subprocess
import sys
import unittest
from unittest import mock

import gate_f1_20 as gate

# Imprime uma linha, descarrega o buffer e fica preso além do timeout.
HANG_AFTER_PRINT = (
    "import sys, time\n"
    "print('[gate] antes do travamento', flush=True)\n"
    "time.sleep(30)\n"
)


class RunTimeoutTests(unittest.TestCase):
    def test_timeout_keeps_output_printed_before_the_hang(self) -> None:
        code, out = gate.run([sys.executable, "-c", HANG_AFTER_PRINT], timeout_s=2)
        self.assertEqual(code, 124)
        self.assertIn("[gate] antes do travamento", out)
        self.assertIn("TIMEOUT depois de 2 s", out)

    def test_timeout_with_bytes_output_is_decoded_not_dropped(self) -> None:
        # No POSIX, TimeoutExpired.stdout chega como bytes mesmo com text=True (causa do CI do Ubuntu).
        expired = subprocess.TimeoutExpired(["godot"], 120, output=b"[gate] carregou\n")
        with mock.patch.object(gate.subprocess, "run", side_effect=expired):
            code, out = gate.run(["godot"], timeout_s=120)
        self.assertEqual(code, 124)
        self.assertIn("[gate] carregou", out)

    def test_normal_exit_returns_code_and_output(self) -> None:
        code, out = gate.run([sys.executable, "-c", "print('ok')"], timeout_s=30)
        self.assertEqual(code, 0)
        self.assertIn("ok", out)


if __name__ == "__main__":
    unittest.main()
