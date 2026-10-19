# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Generate vectors-v0.json: signed envelope, Zstd/LZ4 frames and FastCDC chunker (spec 03).

Standard library only. Needs Python 3.14+ (``compression.zstd``) or the ``zstandard`` package.
Run:  python gen_vectors.py            -> rewrites vectors-v0.json next to this file.
The conformance check is done by verify_vectors.py, which does NOT import this module.
"""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path
from typing import Any

try:  # Python 3.14+
    from compression import zstd as _zstd

    def zstd_compress(data: bytes) -> bytes:
        return bytes(_zstd.compress(data, level=3))

except ImportError:  # pragma: no cover - fallback for older interpreters
    import zstandard

    def zstd_compress(data: bytes) -> bytes:
        return bytes(zstandard.ZstdCompressor(level=3, write_content_size=True).compress(data))


OUT = Path(__file__).with_name("vectors-v0.json")
M64 = (1 << 64) - 1

# ---------------------------------------------------------------- deterministic input streams


def stream(label: str, n: int) -> bytes:
    """n bytes of SHA-256(b'swarmstream-vec-v0|' + label + u32le(counter)) blocks, concatenated."""
    out = bytearray()
    counter = 0
    while len(out) < n:
        out += hashlib.sha256(
            b"swarmstream-vec-v0|" + label.encode() + struct.pack("<I", counter)
        ).digest()
        counter += 1
    return bytes(out[:n])


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


# ---------------------------------------------------------------- Ed25519 (RFC 8032, pure Python)

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _recover_x(y: int, sign: int) -> int:
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _I % _P
    if x & 1 != sign:
        x = _P - x
    return x


_BY = 4 * pow(5, _P - 2, _P) % _P
_B = (_recover_x(_BY, 0), _BY, 1, _recover_x(_BY, 0) * _BY % _P)


def _add(p: tuple[int, int, int, int], q: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    a = (p[1] - p[0]) * (q[1] - q[0]) % _P
    b = (p[1] + p[0]) * (q[1] + q[0]) % _P
    c = 2 * p[3] * q[3] * _D % _P
    d = 2 * p[2] * q[2] % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(s: int, p: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _enc(p: tuple[int, int, int, int]) -> bytes:
    zi = pow(p[2], _P - 2, _P)
    x, y = p[0] * zi % _P, p[1] * zi % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _h512(m: bytes) -> int:
    return int.from_bytes(hashlib.sha512(m).digest(), "little")


def ed25519_keys(seed: bytes) -> tuple[int, bytes, bytes]:
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:], _enc(_mul(a, _B))


def ed25519_sign(seed: bytes, msg: bytes) -> bytes:
    a, prefix, pub = ed25519_keys(seed)
    r = _h512(prefix + msg) % _L
    rs = _enc(_mul(r, _B))
    k = _h512(rs + pub + msg) % _L
    return rs + int.to_bytes((r + k * a) % _L, 32, "little")


# ---------------------------------------------------------------- LZ4 block compressor (greedy)


def lz4_block_compress(src: bytes) -> bytes:
    """Valid LZ4 *block* encoding.

    Not optimal; conformance is 'decodes to src', not byte equality.
    """
    n = len(src)
    out = bytearray()
    table: dict[bytes, int] = {}
    anchor = i = 0
    limit = n - 12  # last match must start >= 12 bytes before the end; last 5 bytes are literals

    def emit(lit_start: int, lit_end: int, offset: int, match_len: int) -> None:
        lit = lit_end - lit_start
        token_lit = min(lit, 15)
        token_match = 0 if match_len == 0 else min(match_len - 4, 15)
        out.append(token_lit << 4 | token_match)
        rest = lit - 15
        if lit >= 15:
            while rest >= 255:
                out.append(255)
                rest -= 255
            out.append(rest)
        out.extend(src[lit_start:lit_end])
        if match_len:
            out.extend(struct.pack("<H", offset))
            rest = match_len - 4 - 15
            if match_len - 4 >= 15:
                while rest >= 255:
                    out.append(255)
                    rest -= 255
                out.append(rest)

    while i < limit:
        key = src[i : i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= 0xFFFF:
            length = 4
            while i + length < n - 5 and src[cand + length] == src[i + length]:
                length += 1
            emit(anchor, i, i - cand, length)
            i += length
            anchor = i
        else:
            i += 1
    emit(anchor, n, 0, 0)  # trailing literals (token has no match part)
    return bytes(out)


# ---------------------------------------------------------------- FastCDC chunker (spec 03 §11.1)


def gear_table() -> list[int]:
    return [
        int.from_bytes(sha256(b"swarmstream-gear-v0" + bytes([i]))[:8], "little")
        for i in range(256)
    ]


def _masks(avg: int) -> tuple[int, int]:
    bits = avg.bit_length() - 1

    def high_bits(count: int) -> int:
        return ((1 << count) - 1) << (64 - count)

    return high_bits(bits + 2), high_bits(bits - 2)  # mask_s (stricter), mask_l (looser)


def chunk_lengths(data: bytes, mn: int, avg: int, mx: int) -> list[int]:
    gear = gear_table()
    mask_s, mask_l = _masks(avg)
    lengths: list[int] = []
    pos = 0
    while pos < len(data):
        remaining = len(data) - pos
        if remaining <= mn:
            lengths.append(remaining)
            break
        limit = min(remaining, mx)
        normal = min(avg, limit)
        fp = 0
        i = mn  # hash starts at offset `mn` with fp = 0 (cut-point skipping)
        cut = limit
        while i < limit:
            fp = ((fp << 1) + gear[data[pos + i]]) & M64
            if fp & (mask_s if i < normal else mask_l) == 0:
                cut = i + 1
                break
            i += 1
        lengths.append(cut)
        pos += cut
    return lengths


# ---------------------------------------------------------------- vector builders

SPEC_PARAMS = (32 * 1024, 128 * 1024, 256 * 1024)
KEY_SEED = bytes(range(32))  # TEST KEY ONLY: 00 01 02 ... 1f


def build_chunker() -> dict[str, Any]:
    gear = gear_table()
    cases = []
    specs = [
        ("empty", 0, SPEC_PARAMS),
        ("one_byte", 1, SPEC_PARAMS),
        ("exactly_min", SPEC_PARAMS[0], SPEC_PARAMS),
        ("min_plus_1", SPEC_PARAMS[0] + 1, SPEC_PARAMS),
        ("exactly_max", SPEC_PARAMS[2], SPEC_PARAMS),
        ("max_plus_1", SPEC_PARAMS[2] + 1, SPEC_PARAMS),
        ("random_1mib", 1 << 20, SPEC_PARAMS),
        ("random_4mib", 4 << 20, SPEC_PARAMS),
        ("small_params_64k", 64 * 1024, (256, 1024, 4096)),
    ]
    for label, n, (mn, avg, mx) in specs:
        data = stream("chunker/" + label, n)
        lengths = chunk_lengths(data, mn, avg, mx)
        offs, chunks = 0, []
        for length in lengths:
            chunks.append({"len": length, "sha256": sha256(data[offs : offs + length]).hex()})
            offs += length
        cases.append(
            {
                "label": label,
                "stream": "chunker/" + label,
                "size": n,
                "min": mn,
                "avg": avg,
                "max": mx,
                "data_sha256": sha256(data).hex(),
                "chunks": chunks,
            }
        )
    zeros = bytes(2 << 20)
    cases.append(
        {
            "label": "zeros_2mib",
            "zeros": True,
            "size": len(zeros),
            "min": SPEC_PARAMS[0],
            "avg": SPEC_PARAMS[1],
            "max": SPEC_PARAMS[2],
            "data_sha256": sha256(zeros).hex(),
            "chunks": [
                {"len": n, "sha256": sha256(zeros[:n]).hex()}
                for n in chunk_lengths(zeros, *SPEC_PARAMS)
            ],
        }
    )
    return {
        "algorithm": "swarmstream-fastcdc-v0",
        "gear_first4_hex": [format(g, "016x") for g in gear[:4]],
        "gear_table_sha256": sha256(b"".join(struct.pack("<Q", g) for g in gear)).hex(),
        "cases": cases,
    }


def build_codecs() -> dict[str, Any]:
    inputs: list[tuple[str, bytes]] = [
        ("zeros_4096", bytes(4096)),
        ("repeat_pattern_8192", (b"swarmstream-" * 700)[:8192]),
        ("text_like_6000", (b"The quick brown fox jumps over the lazy dog. " * 140)[:6000]),
        ("incompressible_1000", stream("codec/incompressible", 1000)),
        ("one_byte", b"\x42"),
        ("max_block_zeros_262144", bytes(262144)),
    ]
    cases = []
    for label, raw in inputs:
        entry: dict[str, Any] = {
            "label": label,
            "raw_size": len(raw),
            "raw_sha256": sha256(raw).hex(),
            "raw_source": (
                {"zeros": len(raw)}
                if raw == bytes(len(raw))
                else (
                    {"stream": "codec/incompressible", "n": len(raw)}
                    if label == "incompressible_1000"
                    else {"hex": raw.hex()}
                )
            ),
        }
        for name, codec, blob in (
            ("zstd", 1, zstd_compress(raw)),
            ("lz4", 2, lz4_block_compress(raw)),
        ):
            # spec §4: if compression does not shrink the block, the packer MUST use NONE
            entry[name] = {
                "codec_value": codec,
                "packed_size": len(blob),
                "must_use_none": len(blob) >= len(raw),
                "frame_hex": blob.hex(),
            }
        cases.append(entry)
    return {"cases": cases}


def build_envelope() -> dict[str, Any]:
    _, _, pub = ed25519_keys(KEY_SEED)
    key_id = sha256(pub)[:8]
    payload = b"SWM0-synthetic-payload-v0:" + stream("envelope/payload", 70)

    def envelope_head(payload_len: int, key: bytes = key_id, version: int = 0) -> bytes:
        return (
            b"SWMANIF\x00"
            + struct.pack("<H", version)  # format_version (versão do envelope)
            + bytes([1, 1])  # hash_alg SHA-256, sig_alg Ed25519
            + struct.pack("<I", 0)  # reserved
            + struct.pack("<Q", payload_len)
            + key
            + struct.pack("<Q", 0)  # reserved2
        )

    head = envelope_head(len(payload))
    if len(head) != 40:
        raise RuntimeError("envelope head must be 40 bytes")
    msg = b"SWARMSTREAM-MANIFEST-V0\x00" + head + payload
    sig = ed25519_sign(KEY_SEED, msg)
    manifest = head + sig + payload
    manifest_id = sha256(manifest[:104] + payload)

    other_sig = ed25519_sign(KEY_SEED, msg + b"x")  # valid signature, wrong message
    flipped_payload = bytearray(manifest)
    flipped_payload[104 + 10] ^= 0x01
    flipped_hash = bytearray(manifest)  # "one byte of a hash": a byte deep in the payload region
    flipped_hash[104 + 40] ^= 0x80
    bad_magic = bytearray(manifest)
    bad_magic[0] ^= 0x01
    bad_reserved = bytearray(manifest)
    bad_reserved[12] = 1
    truncated = manifest[:-1]
    unknown_key = bytearray(manifest)
    unknown_key[24] ^= 0xFF

    def signed(body: bytes, version: int = 0) -> bytes:
        h = envelope_head(len(body), version=version)
        return h + ed25519_sign(KEY_SEED, b"SWARMSTREAM-MANIFEST-V0\x00" + h + body) + body

    # Envelope de versão desconhecida, com assinatura válida: recusado por igualdade (§7.1 passo 1).
    future_envelope = signed(payload, version=1)
    # Versão desconhecida E reservado != 0: a versão é conferida antes do resto (§7.1 passo 1b).
    future_and_reserved = bytearray(future_envelope)
    future_and_reserved[12] = 1

    return {
        "test_key_seed_hex": KEY_SEED.hex(),
        "test_public_key_hex": pub.hex(),
        "key_id_hex": key_id.hex(),
        "payload_note": (
            "synthetic opaque payload; a real FlatBuffers payload needs flatc / sw-pack (pending)"
        ),
        "payload_hex": payload.hex(),
        "envelope_head_hex": head.hex(),
        "msg_hex": msg.hex(),
        "signature_hex": sig.hex(),
        "manifest_hex": manifest.hex(),
        "manifest_id_hex": manifest_id.hex(),
        "reject_cases": [
            {
                "label": "payload_byte_flipped",
                "expect": "signature",
                "manifest_hex": bytes(flipped_payload).hex(),
            },
            {
                "label": "hash_byte_flipped",
                "expect": "signature",
                "manifest_hex": bytes(flipped_hash).hex(),
            },
            {
                "label": "signature_of_other_message",
                "expect": "signature",
                "manifest_hex": (head + other_sig + payload).hex(),
            },
            {"label": "bad_magic", "expect": "header", "manifest_hex": bytes(bad_magic).hex()},
            {
                "label": "reserved_nonzero",
                "expect": "header",
                "manifest_hex": bytes(bad_reserved).hex(),
            },
            {"label": "truncated_by_one_byte", "expect": "header", "manifest_hex": truncated.hex()},
            {"label": "unknown_key_id", "expect": "key", "manifest_hex": bytes(unknown_key).hex()},
            {
                "label": "format_version_unknown",
                "expect": "unsupported",
                "manifest_hex": future_envelope.hex(),
            },
            {
                "label": "format_version_unknown_and_reserved_nonzero",
                "expect": "unsupported",
                "manifest_hex": bytes(future_and_reserved).hex(),
            },
        ],
        "payload_probe": build_payload_probe(signed),
    }


def minimal_flatbuffer(min_reader: int, ident: bytes = b"SWM0") -> bytes:
    """ManifestPayload só com min_reader (id 4): o mínimo para a sondagem do §7.1, passo 4."""
    vtable = struct.pack("<HH5H", 14, 8, 0, 0, 0, 0, 4)  # 4 + 2*5 bytes; tabela de 8 bytes
    table_at = 8 + len(vtable) + 2  # 24: alinhado a 4
    table = struct.pack("<iHH", table_at - 8, min_reader, 0)  # soffset até a vtable; campo; padding
    return struct.pack("<I", table_at) + ident + vtable + b"\x00\x00" + table


def build_payload_probe(signed: Any) -> dict[str, Any]:
    """Manifestos assinados com payload FlatBuffers mínimo, para a sondagem do min_reader."""
    cases = [
        ("min_reader_0", minimal_flatbuffer(0), "ok", 0),
        ("min_reader_1", minimal_flatbuffer(1), "unsupported", 1),
        # tabela raiz sem nenhum campo (vtable de 4 bytes): min_reader assume o padrão 0
        ("min_reader_absent", struct.pack("<I", 16) + b"SWM0" + struct.pack("<HH", 4, 4)
         + b"\x00" * 4 + struct.pack("<i", 16 - 8), "ok", 0),
        ("wrong_file_identifier", minimal_flatbuffer(0, b"XXXX"), "invalid", None),
        ("root_offset_out_of_range", struct.pack("<I", 4096) + minimal_flatbuffer(0)[4:], "invalid", None),
        # raiz dentro do cabeçalho (r < 8): proibido pela regra r >= 8
        ("root_inside_header", struct.pack("<I", 4) + minimal_flatbuffer(0)[4:], "invalid", None),
        # slot do id 4 aponta para o offset 2 da tabela, por cima do soffset: proibido por fo >= 4
        ("field_overlaps_soffset", minimal_flatbuffer(0)[:8] + struct.pack("<HH5H", 14, 8, 0, 0, 0, 0, 2)
         + minimal_flatbuffer(0)[22:], "invalid", None),
    ]
    return {
        "supported_payload_revision": 0,
        "note": "payload FlatBuffers mínimo (só min_reader, id 4); exercita só o passo 4 do §7.1",
        "cases": [
            {
                "label": label,
                "payload_hex": body.hex(),
                "manifest_hex": signed(body).hex(),
                "expect": expect,
                "min_reader": value,
            }
            for label, body, expect, value in cases
        ],
    }


def main() -> None:
    # self-check: RFC 8032 section 7.1, TEST 1 (empty message)
    seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    rfc8032_test1 = (
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
    )
    if ed25519_sign(seed, b"").hex() != rfc8032_test1:
        raise RuntimeError("Ed25519 self-check failed")
    doc = {
        "version": 0,
        "spec": "docs/03-formato-de-conteudo.md",
        "chunker": build_chunker(),
        "codecs": build_codecs(),
        "envelope": build_envelope(),
    }
    OUT.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8", newline="\n")
    print("wrote", OUT, OUT.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
