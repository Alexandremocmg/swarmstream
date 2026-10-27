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
     crash_test.py <sw-core-crash-tool> --quedas <N> [semente]   (F1-31: roda até N quedas)
     crash_test.py <sw-core-crash-tool> 1 --iteration-seed <semente da iteração>
Toda falha imprime a iteração, a semente dela, os comandos das rodadas e o fim do log.
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


class RoundFailed(RuntimeError):
    """Uma rodada de escrita terminou de um jeito que não é queda nem sucesso."""


def run_round(tool: str, work: Path, log: Path, rng: random.Random) -> str:
    """Roda uma rodada de escrita e devolve a descrição dela (com o comando, para reproduzir)."""
    seed = rng.randrange(1, 2**31)
    if rng.random() < 0.6:
        point = rng.randint(1, CRASH_POINTS)
        count = rng.randint(1, 12)
        args = [tool, "write", str(work), str(seed), str(point), str(count), str(OPS)]
        with log.open("a", encoding="utf-8") as out:
            done = subprocess.run(args, stdout=out, stderr=STDOUT, timeout=300)  # noqa: S603
        what = f"write seed={seed} ponto={point} vez={count}"
        if done.returncode not in (0, 77):
            raise RoundFailed(f"{what}: saiu com código {done.returncode} (esperado 0 ou 77)")
        return f"{what} -> {'queda' if done.returncode == 77 else 'sem queda'}"
    delay = rng.uniform(0.01, 0.4)
    args = [tool, "write", str(work), str(seed), "0", "0", str(KILL_OPS)]
    with log.open("a", encoding="utf-8") as out:
        proc = subprocess.Popen(args, stdout=out, stderr=STDOUT)  # noqa: S603
        time.sleep(delay)
        killed = proc.poll() is None
        if killed:
            proc.kill()
        code = proc.wait(timeout=300)
    what = f"write seed={seed} kill={delay * 1000:.0f}ms"
    if not killed and code != 0:
        raise RoundFailed(f"{what}: saiu com código {code} antes do kill")
    return f"{what} -> {'morto' if killed else 'já tinha terminado'}"


def tail(path: Path, lines: int) -> str:
    try:
        return "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:])
    except OSError as e:
        return f"(log ilegível: {e})"


def report_failure(
    it: int, it_seed: int, tool: str, notes: list[str], base: Path, log: Path, why: str
) -> None:
    """Tudo o que é preciso para reproduzir e entender uma falha (revisão da R2 do PR #34)."""
    print(f"FALHA na iteração {it} (semente da iteração {it_seed}): {why}")
    for k, note in enumerate(notes):
        print(f"  rodada {k + 1}: {note}")
    print("  reproduza com:")
    print(f"    python crash_test.py {tool} 1 --iteration-seed {it_seed}")
    print("  (as rodadas com ponto de queda se repetem iguais; as com kill dependem do tempo)")
    print(f"  últimas linhas do log das rodadas ({log}):")
    print("    " + tail(log, 30).replace("\n", "\n    "))
    print(f"  pasta mantida para análise: {base}")


def run_iteration(tool: str, it: int, it_seed: int) -> tuple[bool, int, int, str]:
    """Uma iteração (pasta nova, 3 rodadas, verify). Devolve (ok, quedas, mortos, resumo)."""
    rng = random.Random(it_seed)  # noqa: S311 (teste: sorteio reproduzível, não segurança)
    base = Path(tempfile.mkdtemp(prefix="swarmstream_f1_11_crash_"))
    work = base / "cache"
    work.mkdir()
    log = base / "ops.log"
    log.touch()
    notes: list[str] = []
    crashes = 0
    kills = 0
    try:
        for _ in range(ROUNDS):
            note = run_round(tool, work, log, rng)
            notes.append(note)
            crashes += 1 if note.endswith("-> queda") else 0
            kills += 1 if note.endswith("-> morto") else 0
        verify = subprocess.run(  # noqa: S603 (teste: executável nosso)
            [tool, "verify", str(work), str(log)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=300,
        )
    except (RoundFailed, subprocess.TimeoutExpired, OSError) as e:
        report_failure(it, it_seed, tool, notes, base, log, f"{type(e).__name__}: {e}")
        return False, crashes, kills, ""
    if verify.returncode != 0:
        out = (verify.stdout + verify.stderr).strip() or "(verify não imprimiu nada)"
        report_failure(
            it, it_seed, tool, notes, base, log, f"verify saiu com código {verify.returncode}"
        )
        print("  saída do verify:")
        print("    " + out.replace("\n", "\n    "))
        return False, crashes, kills, ""
    shutil.rmtree(base, ignore_errors=True)
    return True, crashes, kills, f"{verify.stdout.strip()} [{'; '.join(notes)}]"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 64
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    tool = sys.argv[1]
    target_crashes = 0
    if len(sys.argv) >= 3 and sys.argv[2] == "--quedas":
        target_crashes = int(sys.argv[3])
        if target_crashes < 1:
            print("--quedas precisa de N >= 1")
            return 64
        base_seed = int(sys.argv[4]) if len(sys.argv) > 4 else 20261026
        # teto: pelo menos 1 queda por iteração
        seeds = [base_seed * 1_000_003 + it for it in range(target_crashes)]
    elif len(sys.argv) == 5 and sys.argv[3] == "--iteration-seed":
        seeds = [int(sys.argv[4])]  # reproduz uma iteração que falhou
        base_seed = seeds[0]
    else:
        iterations = int(sys.argv[2]) if len(sys.argv) > 2 else 100
        base_seed = int(sys.argv[3]) if len(sys.argv) > 3 else 20261024
        seeds = [base_seed * 1_000_003 + it for it in range(iterations)]
    print(f"teste de queda: {len(seeds)} iterações, semente base {base_seed}", flush=True)
    crashes = 0
    kills = 0
    t0 = time.monotonic()
    done = 0
    for it, it_seed in enumerate(seeds):
        if target_crashes and crashes + kills >= target_crashes:
            break
        ok, c, k, summary = run_iteration(tool, it, it_seed)
        done += 1
        crashes += c
        kills += k
        if not ok:
            return 1
        if it < 3 or it % 25 == 0:
            print(f"iteração {it} (semente {it_seed}): {summary}", flush=True)
    elapsed = time.monotonic() - t0
    if target_crashes and crashes + kills < target_crashes:
        print(f"FALHA: só {crashes + kills} quedas em {done} iterações (alvo {target_crashes})")
        return 1
    print(
        f"{done} iterações, {done * ROUNDS} rodadas: {crashes} quedas em ponto "
        f"exato, {kills} processos mortos; 0 peças corrompidas ({elapsed:.1f} s, "
        f"semente base {base_seed})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
