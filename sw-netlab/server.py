# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""HTTP/1.1 file server with deterministic bandwidth and fault profiles."""

from __future__ import annotations

import argparse
import math
import socket
import threading
import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from random import SystemRandom
from urllib.parse import unquote, urlsplit


@dataclass(frozen=True)
class NetworkProfile:
    bandwidth_mbps: float
    latency_ms: float
    loss_percent: float


PROFILES = {
    "fibra_300": NetworkProfile(300.0, 10.0, 0.0),
    "cabo_50": NetworkProfile(50.0, 25.0, 0.2),
    "adsl_10": NetworkProfile(10.0, 60.0, 1.0),
}


def wait_until(deadline: float) -> None:
    """Wait until a perf_counter deadline without spinning (spinning starves the GIL)."""
    while (remaining := deadline - time.perf_counter()) > 0.001:
        time.sleep(remaining - 0.0005)


def wait_latency(milliseconds: float) -> None:
    """Wait for the requested delay."""
    wait_until(time.perf_counter() + milliseconds / 1000)


SLICE_BYTES = 64 * 1024


class LinkShaper:
    """One shared link: all connections draw from the same serialized byte budget."""

    def __init__(self, bandwidth_mbps: float) -> None:
        self._seconds_per_byte = 8 / (bandwidth_mbps * 1_000_000)
        self._lock = threading.Lock()
        self._free_at = 0.0
        self._active = 0

    @contextmanager
    def transfer(self) -> Iterator[None]:
        """Mark a body as in flight; the first one on an idle link starts a fresh schedule."""
        with self._lock:
            if self._active == 0:
                self._free_at = time.perf_counter()
            self._active += 1
        try:
            yield
        finally:
            with self._lock:
                self._active -= 1

    def pace(self, nbytes: int) -> None:
        """Block until nbytes have fully crossed the link; call this before writing them."""
        with self._lock:
            # A late wake-up (timer jitter) keeps its place in the schedule: restarting at "now"
            # would lose bandwidth on every slice. Idle links are reset in transfer().
            self._free_at += nbytes * self._seconds_per_byte
            finish = self._free_at
        wait_until(finish)


def make_handler(
    root: Path, profile: NetworkProfile, shaper: LinkShaper | None = None
) -> type[BaseHTTPRequestHandler]:
    """Create an HTTP handler bounded to root and shaped to profile (one shared link)."""
    link = shaper if shaper is not None else LinkShaper(profile.bandwidth_mbps)

    class RangeHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "SwarmstreamNetlab/1.0"

        def setup(self) -> None:
            super().setup()
            # Sliced writes must reach the wire at once; Nagle would batch and delay them.
            self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        def do_GET(self) -> None:
            path = self._resolve_path()
            if path is None or not path.is_file():
                self.send_error(404)
                return
            size = path.stat().st_size
            start, end = 0, size - 1
            range_header = self.headers.get("Range")
            status = 200
            if range_header:
                parsed = self._parse_range(range_header, size)
                if parsed is None:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                start, end = parsed
                status = 206

            wait_latency(profile.latency_ms)
            if profile.loss_percent and SystemRandom().random() < profile.loss_percent / 100:
                # App-level loss models a dropped HTTP request/connection, not TCP packet loss.
                self.close_connection = True
                return

            length = max(0, end - start + 1)
            self.send_response(status)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length))
            self.send_header("Content-Type", "application/octet-stream")
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            if self.headers.get("Connection", "").lower() == "close":
                self.close_connection = True
            sent = 0
            with link.transfer(), path.open("rb") as source:
                source.seek(start)
                while sent < length:
                    chunk = source.read(min(SLICE_BYTES, length - sent))
                    if not chunk:
                        break
                    link.pace(len(chunk))
                    self.wfile.write(chunk)
                    sent += len(chunk)

        def do_HEAD(self) -> None:
            is_latency_probe = urlsplit(self.path).path == "/__netlab_latency_probe__"
            path = self._resolve_path("/payload.bin" if is_latency_probe else None)
            if path is None or not path.is_file():
                self.send_error(404)
                return
            file_size = path.stat().st_size
            if not is_latency_probe:
                wait_latency(profile.latency_ms)
            self.send_response(204 if is_latency_probe else 200)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", "0" if is_latency_probe else str(file_size))
            self.end_headers()

        def _resolve_path(self, request_target: str | None = None) -> Path | None:
            target = request_target if request_target is not None else self.path
            relative = unquote(urlsplit(target).path).lstrip("/")
            candidate = (root / relative).resolve()
            try:
                candidate.relative_to(root)
            except ValueError:
                return None
            return candidate

        @staticmethod
        def _parse_range(value: str, size: int) -> tuple[int, int] | None:
            if not value.startswith("bytes=") or "," in value or size == 0:
                return None
            spec = value[6:].strip()
            try:
                left, right = spec.split("-", 1)
                if not left:
                    suffix = int(right)
                    if suffix <= 0:
                        return None
                    return max(0, size - suffix), size - 1
                start = int(left)
                end = min(int(right), size - 1) if right else size - 1
            except ValueError:
                return None
            if start < 0 or start >= size or end < start:
                return None
            return start, end

        def log_message(self, fmt: str, *args: object) -> None:
            del fmt, args

    return RangeHandler


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).parent / "fixtures")
    parser.add_argument("--profile", choices=(*PROFILES, "custom"), default="cabo_50")
    parser.add_argument("--bandwidth-mbps", type=float)
    parser.add_argument("--latency-ms", type=float)
    parser.add_argument("--loss-percent", type=float)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    if args.profile == "custom":
        if args.bandwidth_mbps is None or args.latency_ms is None:
            parser.error("custom profile requires --bandwidth-mbps and --latency-ms")
        profile = NetworkProfile(args.bandwidth_mbps, args.latency_ms, args.loss_percent or 0.0)
    else:
        profile = PROFILES[args.profile]
        profile = NetworkProfile(
            args.bandwidth_mbps if args.bandwidth_mbps is not None else profile.bandwidth_mbps,
            args.latency_ms if args.latency_ms is not None else profile.latency_ms,
            args.loss_percent if args.loss_percent is not None else profile.loss_percent,
        )
    if (
        not math.isfinite(profile.bandwidth_mbps)
        or not math.isfinite(profile.latency_ms)
        or not math.isfinite(profile.loss_percent)
        or profile.bandwidth_mbps <= 0
        or profile.latency_ms < 0
        or not 0 <= profile.loss_percent <= 100
    ):
        parser.error("bandwidth must be > 0, latency >= 0, loss between 0 and 100")
    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"document root does not exist: {root}")
    server = ThreadingHTTPServer((args.host, args.port), make_handler(root, profile))
    print(f"Serving {root} at http://{args.host}:{args.port} profile={profile}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
