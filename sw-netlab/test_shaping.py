# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Timing tests: the profile bandwidth is one shared link, paced before bytes are sent."""

from __future__ import annotations

import http.client
import tempfile
import threading
import time
import unittest
from dataclasses import replace
from http.server import ThreadingHTTPServer
from pathlib import Path

from server import PROFILES, make_handler

MIB = 1024 * 1024
TOLERANCE = 0.10
# Fixed cost of Python + loopback (~10 ms, more under load) is large next to the 38 ms of fibra_300.
MIN_SLACK_SECONDS = 0.03


class ShapingTest(unittest.TestCase):
    def _serve(self, name: str, root: Path) -> ThreadingHTTPServer:
        profile = replace(PROFILES[name], loss_percent=0.0)  # loss is not under test here
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(root, profile))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    @staticmethod
    def _fetch(port: int, offset: int, length: int) -> int:
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
        try:
            headers = {"Range": f"bytes={offset}-{offset + length - 1}"}
            connection.request("GET", "/payload.bin", headers=headers)
            return len(connection.getresponse().read())
        finally:
            connection.close()

    def _run_once(self, name: str, pieces: int) -> float:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            (root / "payload.bin").write_bytes(bytes(pieces * MIB))
            server = self._serve(name, root)
            port = server.server_address[1]
            self._fetch(port, 0, 1)  # warm up the connection path and thread start
            sizes = [0] * pieces

            def worker(index: int) -> None:
                sizes[index] = self._fetch(port, index * MIB, MIB)

            threads = [threading.Thread(target=worker, args=(i,)) for i in range(pieces)]
            started = time.perf_counter()
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            elapsed = time.perf_counter() - started
        self.assertEqual(sizes, [MIB] * pieces)
        return elapsed

    def _check(self, name: str, pieces: int) -> None:
        profile = PROFILES[name]
        expected = profile.latency_ms / 1000 + pieces * MIB * 8 / (profile.bandwidth_mbps * 1e6)
        delta = max(expected * TOLERANCE, MIN_SLACK_SECONDS)
        # Machine noise only ever adds time, so short cases keep the best of a few runs.
        attempts = 3 if expected < 1 else 1
        elapsed = min(self._run_once(name, pieces) for _ in range(attempts))
        self.assertAlmostEqual(
            elapsed,
            expected,
            delta=delta,
            msg=f"{name} x{pieces}: {elapsed:.3f}s vs theoretical {expected:.3f}s",
        )

    def test_one_piece_of_1_mib(self) -> None:
        for name in PROFILES:
            with self.subTest(profile=name):
                self._check(name, 1)

    def test_eight_parallel_pieces_share_the_link(self) -> None:
        for name in PROFILES:
            with self.subTest(profile=name):
                self._check(name, 8)


if __name__ == "__main__":
    unittest.main()
