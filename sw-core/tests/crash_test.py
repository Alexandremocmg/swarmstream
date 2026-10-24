# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Teste de queda do cache em disco (F1-11; o F1-31 roda o mesmo com 1.000 quedas).

Cada iteração usa uma pasta nova e faz 3 rodadas de escrita na mesma pasta, cada uma morrendo
de um jeito:
- num ponto exato da escrita ou do checkpoint (o processo chama _Exit no ponto sorteado), ou
- morto pelo driver (kill) num instante aleatório.
Depois da última rodada, `sw-core-crash-tool verify` reabre o cache e confere todas as peças
contra o log das 3 rodadas (ver crash_tool.cpp). Qualquer peça corrompida, perdida depois de
confirmada ou presente depois de removida derruba o teste.

Limite honesto: matar o processo não desliga a energia. O que o SO já tinha recebido (mesmo
sem fsync) sobrevive. A ordem das barreiras contra queda de energia é provada por construção
(04 §3) e pelos testes de cauda rasgada e corrupção do journal (sw-core-journal).

Uso: crash_test.py <sw-core-crash-tool> [iterações] [semente]
"""

from __future__ import annotations

import io
import random
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from subprocess import STDOUT

CRASH_POINTS = 8  # detail::CrashPoint 1..8
ROUNDS = 3
OPS = 60  # rodadas que morrem num ponto exato
KILL_OPS = 2000  # rodadas mortas pelo driver: longas o bastante para o kill pegar no meio


def run_round(tool: str, work: Path, log: Path, rng: random.Random) -> str:
    seed = rng.randrange(1, 2**31)
    if rng.random() < 0.6:
        point = rng.randint(1, CRASH_POINTS)
        count = rng.randint(1, 12)
        args = [tool, "write", str(work), str(seed), str(point), str(count), str(OPS)]
        with log.open("a", encoding="utf-8") as out:
            done = subprocess.run(args, stdout=out, stderr=STDOUT, timeout=300)  # noqa: S603
        if done.returncode not in (0, 77):
            raise RuntimeError(f"write terminou com {done.returncode} (ponto {point}, vez {count})")
        return f"ponto {point}x{count} -> {'queda' if done.returncode == 77 else 'sem queda'}"
    delay = rng.uniform(0.01, 0.4)
    args = [tool, "write", str(work), str(seed), "0", "0", str(KILL_OPS)]
    with log.open("a", encoding="utf-8") as out:
        proc = subprocess.Popen(args, stdout=out, stderr=STDOUT)  # noqa: S603
        time.sleep(delay)
        killed = proc.poll() is None
        if killed:
            proc.kill()
        code = proc.wait(timeout=300)
    if not killed and code != 0:
        raise RuntimeError(f"write terminou com {code} antes do kill")
    return f"kill em {delay * 1000:.0f} ms -> {'morto' if killed else 'já tinha terminado'}"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 64
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    tool = sys.argv[1]
    iterations = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 20261024
    rng = random.Random(seed)  # noqa: S311 (teste: executável e sorteio nossos)
    crashes = 0
    kills = 0
    t0 = time.monotonic()
    for it in range(iterations):
        base = Path(tempfile.mkdtemp(prefix="swarmstream_f1_11_crash_"))
        work = base / "cache"
        work.mkdir()
        log = base / "ops.log"
        log.touch()
        try:
            notes = []
            for _ in range(ROUNDS):
                note = run_round(tool, work, log, rng)
                notes.append(note)
                if note.endswith("-> queda"):
                    crashes += 1
                if note.endswith("-> morto"):
                    kills += 1
            verify = subprocess.run(  # noqa: S603 (teste: executável e sorteio nossos)
                [tool, "verify", str(work), str(log)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=300,
            )
            if verify.returncode != 0:
                print(f"iteração {it}: FALHOU ({'; '.join(notes)})")
                print(verify.stdout + verify.stderr)
                print(f"pasta mantida para análise: {base}")
                return 1
            if it < 3 or it % 25 == 0:
                print(f"iteração {it}: {verify.stdout.strip()} [{'; '.join(notes)}]")
        except Exception:
            print(f"pasta mantida para análise: {base}")
            raise
        shutil.rmtree(base, ignore_errors=True)
    elapsed = time.monotonic() - t0
    print(
        f"{iterations} iterações, {iterations * ROUNDS} rodadas: {crashes} quedas em ponto "
        f"exato, {kills} processos mortos; 0 peças corrompidas ({elapsed:.1f} s, semente {seed})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
