# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Measure all built-in profiles against a generated local payload."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path
from statistics import median

from server import PROFILES, make_handler


def fetch_with_resume(base_url: str, size: int) -> tuple[bytes, int, float]:
    """Fetch a complete byte range, resuming when the loss model drops a request."""
    received = bytearray()
    status = 0
    retries = 0
    body_seconds = 0.0
    while len(received) < size:
        request = urllib.request.Request(
            base_url,
            headers={"Range": f"bytes={len(received)}-{size - 1}", "Connection": "close"},
        )
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                status = response.status
                body_started = time.perf_counter()
                received.extend(response.read())
                body_seconds += time.perf_counter() - body_started
        except http.client.IncompleteRead as error:
            body_seconds += time.perf_counter() - body_started
            received.extend(error.partial)
            retries += 1
        except (OSError, urllib.error.URLError):
            retries += 1
        if retries > 10:
            raise RuntimeError(f"download could not complete after {retries} retries")
        if retries:
            time.sleep(0.05)
    return bytes(received), status, body_seconds


def measure_latency(port: int) -> float:
    """Measure server latency after subtracting HTTP handler overhead."""
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    baseline_samples: list[float] = []
    delayed_samples: list[float] = []
    try:
        for index in range(7):
            started = time.perf_counter()
            connection.request("HEAD", "/__netlab_latency_probe__")
            response = connection.getresponse()
            response.read()
            elapsed_ms = (time.perf_counter() - started) * 1000
            if response.status != 204:
                raise RuntimeError(f"latency probe returned HTTP {response.status}")
            if index >= 2:
                baseline_samples.append(elapsed_ms)
        for index in range(7):
            started = time.perf_counter()
            connection.request("HEAD", "/payload.bin")
            response = connection.getresponse()
            response.read()
            elapsed_ms = (time.perf_counter() - started) * 1000
            if response.status != 200:
                raise RuntimeError(f"latency probe returned HTTP {response.status}")
            if index >= 2:
                delayed_samples.append(elapsed_ms)
    finally:
        connection.close()
    return max(0.0, median(delayed_samples) - median(baseline_samples))


def verify_partial_range(port: int, size: int, expected: bytes) -> bool:
    """Verify a bounded range response before the full-throughput sample."""
    start = 123
    end = start + len(expected) - 1
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/payload.bin",
        headers={"Range": f"bytes={start}-{end}", "Connection": "close"},
    )
    with urllib.request.urlopen(request, timeout=10) as response:  # noqa: S310
        body = response.read()
        content_range = response.headers.get("Content-Range")
        return (
            response.status == 206
            and isinstance(content_range, str)
            and content_range == f"bytes {start}-{end}/{size}"
            and body == expected
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size-mib", type=int, default=64)
    parser.add_argument("--tolerance-percent", type=float, default=10.0)
    args = parser.parse_args()
    if args.size_mib <= 0:
        parser.error("--size-mib must be positive")
    passed = True
    with tempfile.TemporaryDirectory(prefix="sw-netlab-") as temp_dir:
        root = Path(temp_dir)
        payload = root / "payload.bin"
        block = bytes(range(256)) * 4096
        size = args.size_mib * 1024 * 1024
        with payload.open("wb") as output:
            remaining = size
            while remaining:
                chunk = block[: min(len(block), remaining)]
                output.write(chunk)
                remaining -= len(chunk)
        expected_hash = hashlib.sha256(payload.read_bytes()).hexdigest()
        with payload.open("rb") as source:
            source.seek(123)
            expected_range = source.read(4096)
        for name, profile in PROFILES.items():
            server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(root, profile))
            server.daemon_threads = False
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            url = f"http://127.0.0.1:{server.server_port}/payload.bin"
            try:
                measured_latency_ms = measure_latency(server.server_port)
                range_ok = verify_partial_range(server.server_port, size, expected_range)
                received, status, elapsed = fetch_with_resume(url, size)
            finally:
                server.shutdown()
                server.server_close()
                worker.join(timeout=2)
            measured_mbps = len(received) * 8 / max(elapsed, 1e-9) / 1_000_000
            error_percent = (
                abs(measured_mbps - profile.bandwidth_mbps) / profile.bandwidth_mbps * 100
            )
            latency_error_percent = (
                abs(measured_latency_ms - profile.latency_ms) / profile.latency_ms * 100
            )
            ok = (
                status == 206
                and range_ok
                and len(received) == size
                and hashlib.sha256(received).hexdigest() == expected_hash
                and error_percent <= args.tolerance_percent
                and latency_error_percent <= args.tolerance_percent
            )
            passed &= ok
            print(
                f"{name}: range={'OK' if range_ok else 'FAIL'}, status={status}, "
                f"bytes={len(received)}, body_time={elapsed:.3f}s, "
                f"latency={measured_latency_ms:.2f}ms (configured {profile.latency_ms:.1f}, "
                f"error {latency_error_percent:.2f}%), throughput={measured_mbps:.2f}Mbps "
                f"(configured {profile.bandwidth_mbps:.2f}, error {error_percent:.2f}%) "
                f"{'PASS' if ok else 'FAIL'}"
            )
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
