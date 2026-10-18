# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Conferência do chunker por outro autor (R1).

Escrita só a partir do pseudocódigo do README e do §11.1 da spec, sem ler
gen_vectors.py nem verify_vectors.py. Confere a tabela gear e todos os casos
de `chunker` em vectors-v0.json. Com --stats, mede a distribuição de tamanhos
e a estabilidade (inserir 1 byte) em 64 MiB pseudoaleatórios.

    python -I verify_chunker_r1.py [--stats]
"""

from __future__ import annotations

import hashlib
import json
import random
import statistics
import struct
import sys
from pathlib import Path

M64 = (1 << 64) - 1
GEAR = [
    struct.unpack("<Q", hashlib.sha256(b"swarmstream-gear-v0" + bytes([i])).digest()[:8])[0]
    for i in range(256)
]


def high_mask(bits: int) -> int:
    return (M64 << (64 - bits)) & M64


def chunk_lengths(data: bytes, mn: int, avg: int, mx: int) -> list[int]:
    lg = avg.bit_length() - 1
    if 1 << lg != avg:
        raise ValueError("avg precisa ser potência de 2")
    mask_s, mask_l = high_mask(lg + 2), high_mask(lg - 2)
    out: list[int] = []
    pos = 0
    while pos < len(data):
        n = len(data) - pos
        if n <= mn:
            cut = n
        else:
            limit = min(n, mx)
            normal = min(avg, limit)
            fp, cut = 0, limit
            for i in range(mn, limit):
                fp = ((fp << 1) + GEAR[data[pos + i]]) & M64
                if fp & (mask_s if i < normal else mask_l) == 0:
                    cut = i + 1
                    break
        out.append(cut)
        pos += cut
    return out


def stream(label: str, n: int) -> bytes:
    buf = bytearray()
    counter = 0
    while len(buf) < n:
        buf += hashlib.sha256(
            b"swarmstream-vec-v0|" + label.encode() + struct.pack("<I", counter)
        ).digest()
        counter += 1
    return bytes(buf[:n])


def check(vectors: dict[str, object]) -> bool:
    chunker = vectors["chunker"]
    assert isinstance(chunker, dict)
    table = b"".join(struct.pack("<Q", g) for g in GEAR)
    ok = hashlib.sha256(table).hexdigest() == chunker["gear_table_sha256"]
    ok &= [f"{g:016x}" for g in GEAR[:4]] == chunker["gear_first4_hex"]
    print("gear:", "ok" if ok else "DIVERGE")
    for case in chunker["cases"]:
        data = stream(case["stream"], case["size"]) if case.get("stream") else bytes(case["size"])
        if hashlib.sha256(data).hexdigest() != case["data_sha256"]:
            print(case["label"], "DADOS DIVERGEM")
            ok = False
            continue
        got: list[tuple[int, str]] = []
        pos = 0
        for length in chunk_lengths(data, case["min"], case["avg"], case["max"]):
            got.append((length, hashlib.sha256(data[pos : pos + length]).hexdigest()))
            pos += length
        expected = [(c["len"], c["sha256"]) for c in case["chunks"]]
        same = got == expected
        print(case["label"], "ok" if same else "DIVERGE")
        ok &= same
    return ok


def stats() -> None:
    rng = random.Random(1)
    data = rng.randbytes(64 << 20)
    lens = chunk_lengths(data, 32768, 131072, 262144)
    print(
        f"blocos={len(lens)} média={statistics.mean(lens) / 1024:.1f} KiB "
        f"mediana={statistics.median(lens) / 1024:.1f} KiB "
        f"no_máximo={100 * sum(n == 262144 for n in lens) / len(lens):.1f}% "
        f"até_avg={100 * sum(n <= 131072 for n in lens) / len(lens):.1f}%"
    )

    def hashes(buf: bytes) -> list[bytes]:
        out, pos = [], 0
        for length in chunk_lengths(buf, 32768, 131072, 262144):
            out.append(hashlib.sha256(buf[pos : pos + length]).digest())
            pos += length
        return out

    mid = len(data) // 2
    before = set(hashes(data))
    after = hashes(data[:mid] + b"\x00" + data[mid:])
    print("blocos novos após inserir 1 byte no meio:", sum(h not in before for h in after))


def main() -> int:
    vectors = json.loads((Path(__file__).parent / "vectors-v0.json").read_text(encoding="utf-8"))
    ok = check(vectors)
    if "--stats" in sys.argv:
        stats()
    print("TODOS OK" if ok else "HÁ DIVERGÊNCIA")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
