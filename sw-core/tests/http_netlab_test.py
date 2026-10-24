# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Baixa peças do servidor da bancada (sw-netlab) com a fonte HTTP do sw-core, num perfil de rede.

Uso: http_netlab_test.py <http_fetch_tool> <server.py> <perfil> [conexoes]

Cria 8 peças de 1 MiB aleatórias (nome = SHA-256 do conteúdo) em <tmp>/p/, sobe o servidor da
bancada com o perfil, roda o http_fetch_tool e confere que cada peça baixada é idêntica à original.
Também confere que o perfil de rede foi mesmo aplicado: o tempo não pode ser menor que o piso
teórico da banda do perfil (senão a bancada não estaria limitando nada).
"""

from __future__ import annotations

import hashlib
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PIECES = 8
PIECE_BYTES = 1024 * 1024
BANDWIDTH_MBPS = {"fibra_300": 300.0, "cabo_50": 50.0, "adsl_10": 10.0}


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def wait_for_port(port: int, server: subprocess.Popen[bytes], seconds: float = 15.0) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise SystemExit(f"o servidor da bancada terminou cedo (codigo {server.returncode})")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.1)
    raise SystemExit("o servidor da bancada nao abriu a porta a tempo")


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    tool, server_script, profile = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    connections = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    if profile not in BANDWIDTH_MBPS:
        raise SystemExit(f"perfil desconhecido: {profile}")

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp).resolve() / "fixtures"
        out = Path(temp).resolve() / "out"
        (root / "p").mkdir(parents=True)
        out.mkdir()
        originals: dict[str, bytes] = {}
        for _ in range(PIECES):
            data = os.urandom(PIECE_BYTES)
            originals[hashlib.sha256(data).hexdigest()] = data
        for name, data in originals.items():
            (root / "p" / name).write_bytes(data)

        port = free_port()
        server = subprocess.Popen(
            [
                sys.executable,
                str(server_script),
                "--root",
                str(root),
                "--profile",
                profile,
                "--port",
                str(port),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_for_port(port, server)
            started = time.monotonic()
            result = subprocess.run(
                [tool, f"http://127.0.0.1:{port}", str(out), str(connections), *originals],
                capture_output=True,
                text=True,
                timeout=180,
                check=False,
            )
            elapsed = time.monotonic() - started
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()

        print(result.stdout.strip())
        if result.stderr.strip():
            print(result.stderr.strip(), file=sys.stderr)
        if result.returncode != 0:
            print(f"FALHA: http_fetch_tool saiu com {result.returncode}", file=sys.stderr)
            return 1
        for name, data in originals.items():
            got = (out / f"{name}.bin").read_bytes() if (out / f"{name}.bin").exists() else b""
            if got != data:
                print(
                    f"FALHA: peca {name[:12]}... diferente da original ({len(got)} bytes)",
                    file=sys.stderr,
                )
                return 1

        total_mib = PIECES * PIECE_BYTES / (1024 * 1024)
        # A banda do perfil vale por resposta HTTP; com N conexões o piso é total / (N * banda).
        floor = (total_mib * 8 * 1.048576) / (min(connections, PIECES) * BANDWIDTH_MBPS[profile])
        print(
            f"perfil={profile} pecas={PIECES} conexoes={connections} tempo={elapsed:.2f}s "
            f"taxa={total_mib / elapsed:.1f}MiB/s piso_teorico={floor:.2f}s"
        )
        # Piso frouxo (40%): o servidor da bancada aplica a banda por bloco de 1 MiB e DEPOIS de enviá-lo,
        # então uma peça de 1 MiB chega inteira antes do atraso (ele só segura a próxima requisição da
        # mesma conexão). Ver o PR: o limite real fica ~1,6x acima do nominal em adsl_10.
        if elapsed < floor * 0.4:
            print(
                "FALHA: baixou mais rapido que o perfil permite; a bancada nao esta limitando",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
