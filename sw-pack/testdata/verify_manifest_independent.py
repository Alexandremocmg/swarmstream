#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Confere um manifesto v0 gerado pelo sw-pack com bibliotecas de terceiros (F1-04).

Nada daqui usa código do sw-pack:
  * a assinatura é verificada com `cryptography` (OpenSSL);
  * o manifest_id é o SHA-256 do `hashlib`;
  * o payload é lido com o runtime OFICIAL do FlatBuffers para Python (sem código gerado),
    pelos ids de campo do esquema de docs/03 §7.2, e as validações principais do §8 são refeitas.

Uso:  python -I verify_manifest_independent.py manifest.bin
      (gere um com: sw-manifest-bench generate --blocks 3000 --out manifest.bin)
Precisa de:  pip install flatbuffers cryptography
"""

import hashlib
import struct
import sys

import flatbuffers
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from flatbuffers import number_types as N
from flatbuffers.table import Table

TEST_SEED = bytes(range(32))  # a chave de teste de sw-pack/testdata (ADR-0012)
PREFIX = b"SWARMSTREAM-MANIFEST-V0\x00"


def field_table(tab: Table, field_id: int):
    o = tab.Offset(4 + 2 * field_id)
    if not o:
        return None
    return Table(tab.Bytes, tab.Indirect(tab.Pos + o))


def vector(tab: Table, field_id: int):
    """(início, comprimento) do vetor do campo, ou (0, 0) se ausente."""
    o = tab.Offset(4 + 2 * field_id)
    if not o:
        return 0, 0
    return tab.Vector(o), tab.VectorLen(o)


def u32s(tab, field_id):
    start, n = vector(tab, field_id)
    return [tab.Get(N.Uint32Flags, start + 4 * i) for i in range(n)]


def u16s(tab, field_id):
    start, n = vector(tab, field_id)
    return [tab.Get(N.Uint16Flags, start + 2 * i) for i in range(n)]


def u8s(tab, field_id):
    start, n = vector(tab, field_id)
    return [tab.Get(N.Uint8Flags, start + i) for i in range(n)]


def u64s(tab, field_id):
    start, n = vector(tab, field_id)
    return [tab.Get(N.Uint64Flags, start + 8 * i) for i in range(n)]


def strings(tab, field_id):
    start, n = vector(tab, field_id)
    out = []
    for i in range(n):
        out.append(bytes(tab.String(start + 4 * i)).decode("utf-8"))
    return out


def main() -> int:
    data = open(sys.argv[1], "rb").read()
    assert data[:8] == b"SWMANIF\x00", "magic"
    fmt, hash_alg, sig_alg, reserved, payload_len, key_id, reserved2 = struct.unpack_from("<HBBIQ8sQ", data, 8)
    assert (fmt, hash_alg, sig_alg, reserved, reserved2) == (0, 1, 1, 0, 0)
    assert len(data) == 104 + payload_len, "payload_len"
    signature, payload = data[40:104], data[104:]

    public_key = Ed25519PrivateKey.from_private_bytes(TEST_SEED).public_key()
    raw_pk = public_key.public_bytes_raw()
    assert hashlib.sha256(raw_pk).digest()[:8] == key_id, "key_id"
    try:
        public_key.verify(signature, PREFIX + data[:40] + payload)
    except InvalidSignature:
        print("ASSINATURA NAO CONFERE")
        return 1
    print("assinatura Ed25519 confere (cryptography/OpenSSL)")
    print("manifest_id", hashlib.sha256(data).hexdigest())

    assert payload[4:8] == b"SWM0", "file_identifier"
    root = Table(payload, struct.unpack_from("<I", payload, 0)[0])
    o = root.Offset(4 + 2 * 4)
    assert o and root.Get(N.Uint16Flags, root.Pos + o) == 0, "min_reader"  # revisao 0

    game_id = bytes(root.String(root.Pos + root.Offset(4 + 0))).decode()
    build_seq = root.Get(N.Uint64Flags, root.Pos + root.Offset(4 + 2 * 2))
    blocks, pieces, files = field_table(root, 6), field_table(root, 7), field_table(root, 9)
    n_blocks = vector(blocks, 0)[1]
    raw, packed, codec = u32s(blocks, 1), u32s(blocks, 2), u8s(blocks, 3)
    assert len(raw) == len(packed) == len(codec) == n_blocks
    assert all(1 <= r <= 262144 for r in raw) and all(1 <= p <= 1048576 for p in packed)
    assert all(c <= 2 for c in codec) and all(p == r for r, p, c in zip(raw, packed, codec) if c == 0)

    first, count, size, group = u32s(pieces, 2), u32s(pieces, 3), u32s(pieces, 1), u16s(pieces, 4)
    n_pieces = vector(pieces, 0)[1]
    assert len(first) == len(count) == len(size) == len(group) == n_pieces
    expected_first = 0
    for f, c, s in zip(first, count, size):
        assert f == expected_first
        assert 1 <= s <= 1048576 and s == sum(packed[f : f + c])
        expected_first += c
    assert expected_first == n_blocks

    paths, fsize, fcls = strings(files, 0), u64s(files, 1), u8s(files, 2)
    fgroup, ffirst, fcount = u16s(files, 3), u32s(files, 4), u32s(files, 5)
    refs = u32s(root, 8)
    assert paths == sorted(paths, key=lambda p: p.encode()), "ordem bytewise"
    assert len({p.lower() for p in paths}) == len(paths)
    expected_ref = 0
    for i, p in enumerate(paths):
        assert ffirst[i] == expected_ref
        r = refs[ffirst[i] : ffirst[i] + fcount[i]]
        assert fsize[i] == sum(raw[b] for b in r), p
        assert (fcls[i] == 0) == (fgroup[i] == 0)
        expected_ref += fcount[i]
    assert expected_ref == len(refs)

    gstart, gn = vector(root, 10)
    names = []
    for i in range(gn):
        g = Table(payload, root.Indirect(gstart + 4 * i))
        names.append(bytes(g.String(g.Pos + g.Offset(4))).decode())
    assert names[0] == "core"
    print(f"payload lido com o runtime oficial do FlatBuffers: game_id={game_id} build_seq={build_seq} "
          f"blocos={n_blocks} pecas={n_pieces} arquivos={len(paths)} grupos={names}")
    print("validacoes do 03 secao 8 refeitas em Python: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
