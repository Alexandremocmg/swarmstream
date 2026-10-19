# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Check vectors-v0.json against implementations written from the spec, not from gen_vectors.py.

Independent pieces: Ed25519 by OpenSSL (``openssl`` CLI, deterministic signature compared byte for
byte), LZ4 block decoder and FastCDC chunker rewritten here, a Zstd frame-header parser written
from RFC 8878 (decompression itself uses libzstd via ``compression.zstd`` or ``zstandard``).
Standard library only. Exit code 0 = every vector reproduced.
"""

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

HERE = Path(__file__).parent
M64 = 0xFFFFFFFFFFFFFFFF
ED_PRIV_PREFIX = bytes.fromhex("302e020100300506032b657004220420")  # PKCS#8 for a 32-byte seed
ED_PUB_PREFIX = bytes.fromhex("302a300506032b6570032100")  # SubjectPublicKeyInfo


def sha(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def stream(label: str, n: int) -> bytes:
    out = b""
    c = 0
    while len(out) < n:
        out += sha(b"swarmstream-vec-v0|" + label.encode() + c.to_bytes(4, "little"))
        c += 1
    return out[:n]


# ------------------------------------------------------------------ FastCDC (rewritten)


def chunker_lengths(data: bytes, mn: int, avg: int, mx: int) -> list[int]:
    table = [
        int.from_bytes(sha(b"swarmstream-gear-v0" + bytes((b,)))[:8], "little") for b in range(256)
    ]
    lg = avg.bit_length() - 1
    strict = (M64 << (64 - (lg + 2))) & M64  # lg+2 high bits set
    loose = (M64 << (64 - (lg - 2))) & M64  # lg-2 high bits set
    out: list[int] = []
    start = 0
    total = len(data)
    while start < total:
        left = total - start
        if left <= mn:
            out.append(left)
            break
        end = min(left, mx)
        mid = min(avg, end)
        h = 0
        end_of_chunk = end
        for idx in range(mn, end):
            h = ((h << 1) + table[data[start + idx]]) & M64
            if (h & (strict if idx < mid else loose)) == 0:
                end_of_chunk = idx + 1
                break
        out.append(end_of_chunk)
        start += end_of_chunk
    return out


# ------------------------------------------------------------------ LZ4 block decoder (rewritten)


def lz4_block_decode(src: bytes, cap: int) -> bytes:
    out = bytearray()
    pos = 0
    while True:
        token = src[pos]
        pos += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[pos]
                pos += 1
                lit += b
                if b != 255:
                    break
        out += src[pos : pos + lit]
        if len(src[pos : pos + lit]) != lit:
            raise ValueError("literal run past end")
        pos += lit
        if pos == len(src):
            break  # last sequence has literals only
        offset = int.from_bytes(src[pos : pos + 2], "little")
        pos += 2
        if offset == 0 or offset > len(out):
            raise ValueError("bad match offset")
        mlen = token & 15
        if mlen == 15:
            while True:
                b = src[pos]
                pos += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        for _ in range(mlen):  # byte-wise copy handles overlapping matches
            out.append(out[-offset])
        if len(out) > cap:
            raise ValueError("output exceeds capacity")
    return bytes(out)


# ------------------------------------------------------------------ Zstd frame header (RFC 8878 §3.1.1)


def zstd_header(frame: bytes) -> dict[str, int | None]:
    if frame[:4] != bytes.fromhex("28b52ffd"):
        raise ValueError("not a Zstandard frame (magic)")
    fhd = frame[4]
    fcs_flag, single, dict_flag = fhd >> 6, (fhd >> 5) & 1, fhd & 3
    if fhd & 0x08:
        raise ValueError("reserved bit set")
    pos = 5 + (0 if single else 1)  # Window_Descriptor present unless Single_Segment
    dict_id = int.from_bytes(frame[pos : pos + (0, 1, 2, 4)[dict_flag]], "little")
    pos += (0, 1, 2, 4)[dict_flag]
    fcs_size = (1 if single else 0, 2, 4, 8)[fcs_flag]
    fcs = None
    if fcs_flag or single:
        fcs = int.from_bytes(frame[pos : pos + fcs_size], "little")
        if fcs_flag == 1:
            fcs += 256
    return {"content_size": fcs, "dict_id": dict_id}


def zstd_decompress(frame: bytes) -> bytes:
    try:
        from compression import zstd

        return zstd.decompress(frame)
    except ImportError:
        pass
    try:
        import zstandard
    except ImportError:
        raise SystemExit(
            "no Zstd decoder: use Python 3.14+, or `pip install zstandard`, "
            "or pass --skip-zstd (then Zstd is NOT fully verified)"
        ) from None
    return zstandard.ZstdDecompressor().decompress(frame)


# ------------------------------------------------------------------ OpenSSL Ed25519


def _openssl(*args: str) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(
            ["openssl", *args], capture_output=True, check=False
        )  # noqa: S603,S607
    except FileNotFoundError:
        raise SystemExit(
            "`openssl` (3.x) not found in PATH. Windows: Git for Windows ships it in "
            "<pasta do Git>/usr/bin; Ubuntu: apt install openssl"
        ) from None


def openssl_sign(seed: bytes, msg: bytes) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        key, data, sig = (Path(tmp) / n for n in ("k.der", "m.bin", "s.bin"))
        key.write_bytes(ED_PRIV_PREFIX + seed)
        data.write_bytes(msg)
        r = _openssl(
            "pkeyutl",
            "-sign",
            "-inkey",
            str(key),
            "-keyform",
            "DER",
            "-rawin",
            "-in",
            str(data),
            "-out",
            str(sig),
        )
        if r.returncode:
            raise RuntimeError(r.stderr.decode())
        return sig.read_bytes()


def openssl_pubkey(seed: bytes) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        key = Path(tmp) / "k.der"
        key.write_bytes(ED_PRIV_PREFIX + seed)
        r = _openssl("pkey", "-inform", "DER", "-in", str(key), "-pubout", "-outform", "DER")
        if r.returncode:
            raise RuntimeError(r.stderr.decode())
        return r.stdout[-32:]


def openssl_verify(pub: bytes, msg: bytes, sig: bytes) -> bool:
    with tempfile.TemporaryDirectory() as tmp:
        key, data, sigf = (Path(tmp) / n for n in ("p.der", "m.bin", "s.bin"))
        key.write_bytes(ED_PUB_PREFIX + pub)
        data.write_bytes(msg)
        sigf.write_bytes(sig)
        r = _openssl(
            "pkeyutl",
            "-verify",
            "-pubin",
            "-inkey",
            str(key),
            "-keyform",
            "DER",
            "-rawin",
            "-in",
            str(data),
            "-sigfile",
            str(sigf),
        )
        return r.returncode == 0


# ------------------------------------------------------------------ manifest reader (spec §7.1 order)


def read_envelope(blob: bytes, trusted: dict[bytes, bytes]) -> str:
    """Return 'ok' or the class of the first failed step: header | unsupported | key | signature."""
    if len(blob) < 104 or blob[:8] != b"SWMANIF\x00":
        return "header"
    version, hash_alg, sig_alg, reserved, payload_len, key_id, reserved2 = struct.unpack(
        "<HBBIQ8sQ", blob[8:40]
    )
    if version != 0:  # versão do envelope, por igualdade, antes do resto (§7.1 passo 1b)
        return "unsupported"
    if (hash_alg, sig_alg, reserved, reserved2) != (1, 1, 0, 0):
        return "header"
    if payload_len > 256 * 2**20 or len(blob) != 104 + payload_len:
        return "header"
    pub = trusted.get(key_id)
    if pub is None:
        return "key"
    msg = b"SWARMSTREAM-MANIFEST-V0\x00" + blob[:40] + blob[104:]
    return "ok" if openssl_verify(pub, msg, blob[40:104]) else "signature"


def probe_min_reader(p: bytes) -> int | None:
    """§7.1 passo 4: lê só o min_reader (id 4) com limites conferidos; None = inválido."""
    n = len(p)
    if n < 8 or p[4:8] != b"SWM0":
        return None
    (root,) = struct.unpack_from("<I", p, 0)
    if root < 8 or root % 4 or root + 4 > n:
        return None
    (soff,) = struct.unpack_from("<i", p, root)
    vt = root - soff
    if vt < 0 or vt % 2 or vt + 4 > n:
        return None
    vt_size, tbl_size = struct.unpack_from("<HH", p, vt)
    if vt_size < 4 or vt_size % 2 or vt + vt_size > n or tbl_size < 4 or root + tbl_size > n:
        return None
    slot = 4 + 2 * 4
    field = struct.unpack_from("<H", p, vt + slot)[0] if slot + 2 <= vt_size else 0
    if field == 0:
        return 0
    if field < 4 or field + 2 > tbl_size:
        return None
    return int(struct.unpack_from("<H", p, root + field)[0])


def read_manifest_head(blob: bytes, trusted: dict[bytes, bytes], supported: int) -> str:
    """§7.1 passos 1 a 4: 'ok' | header | unsupported | key | signature | invalid."""
    step = read_envelope(blob, trusted)
    if step != "ok":
        return step
    value = probe_min_reader(blob[104:])
    if value is None:
        return "invalid"
    return "unsupported" if value > supported else "ok"


# ------------------------------------------------------------------ checks


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.checks = 0

    def check(self, ok: bool, what: str) -> None:
        self.checks += 1
        if not ok:
            self.failures.append(what)


def verify_chunker(doc: dict[str, Any], rep: Report) -> None:
    gear = [
        int.from_bytes(sha(b"swarmstream-gear-v0" + bytes((b,)))[:8], "little") for b in range(256)
    ]
    rep.check([format(g, "016x") for g in gear[:4]] == doc["gear_first4_hex"], "gear first 4")
    rep.check(
        sha(b"".join(struct.pack("<Q", g) for g in gear)).hex() == doc["gear_table_sha256"],
        "gear table hash",
    )
    for case in doc["cases"]:
        data = bytes(case["size"]) if case.get("zeros") else stream(case["stream"], case["size"])
        name = f"chunker/{case['label']}"
        rep.check(sha(data).hex() == case["data_sha256"], name + " input hash")
        lengths = chunker_lengths(data, case["min"], case["avg"], case["max"])
        rep.check(lengths == [c["len"] for c in case["chunks"]], name + " chunk lengths")
        off, ok = 0, True
        for c in case["chunks"]:
            ok &= sha(data[off : off + c["len"]]).hex() == c["sha256"]
            off += c["len"]
        rep.check(ok and off == case["size"], name + " chunk hashes cover input")
        rep.check(
            all(c["len"] <= case["max"] for c in case["chunks"])
            and all(c["len"] >= case["min"] for c in case["chunks"][:-1]),
            name + " size bounds",
        )


def verify_codecs(doc: dict[str, Any], rep: Report, skip_zstd: bool) -> None:
    for case in doc["cases"]:
        src = case["raw_source"]
        raw = (
            bytes(src["zeros"])
            if "zeros" in src
            else stream(src["stream"], src["n"]) if "stream" in src else bytes.fromhex(src["hex"])
        )
        name = f"codec/{case['label']}"
        rep.check(
            len(raw) == case["raw_size"] and sha(raw).hex() == case["raw_sha256"],
            name + " raw hash",
        )
        lz4 = case["lz4"]
        frame = bytes.fromhex(lz4["frame_hex"])
        rep.check(len(frame) == lz4["packed_size"], name + " lz4 packed_size")
        try:
            rep.check(
                lz4_block_decode(frame, case["raw_size"]) == raw, name + " lz4 decodes to raw"
            )
        except (ValueError, IndexError) as error:
            rep.check(False, f"{name} lz4 decode error: {error}")
        rep.check(lz4["must_use_none"] == (len(frame) >= len(raw)), name + " lz4 NONE rule")
        zs = case["zstd"]
        zframe = bytes.fromhex(zs["frame_hex"])
        rep.check(len(zframe) == zs["packed_size"], name + " zstd packed_size")
        try:
            header = zstd_header(zframe)
            rep.check(
                header["content_size"] == len(raw),
                name + " zstd Frame_Content_Size present and exact",
            )
            rep.check(header["dict_id"] == 0, name + " zstd Dictionary_ID == 0")
            if not skip_zstd:
                rep.check(zstd_decompress(zframe) == raw, name + " zstd decodes to raw")
        except Exception as error:  # noqa: BLE001 - any decoder error is a failed vector
            rep.check(False, f"{name} zstd error: {error}")
        rep.check(zs["must_use_none"] == (len(zframe) >= len(raw)), name + " zstd NONE rule")


def verify_envelope(doc: dict[str, Any], rep: Report) -> None:
    seed = bytes.fromhex(doc["test_key_seed_hex"])
    pub = openssl_pubkey(seed)
    rep.check(pub.hex() == doc["test_public_key_hex"], "envelope public key (OpenSSL)")
    key_id = sha(pub)[:8]
    rep.check(key_id.hex() == doc["key_id_hex"], "envelope key_id")
    manifest = bytes.fromhex(doc["manifest_hex"])
    payload = bytes.fromhex(doc["payload_hex"])
    head = manifest[:40]
    rep.check(head.hex() == doc["envelope_head_hex"], "envelope head bytes")
    msg = b"SWARMSTREAM-MANIFEST-V0\x00" + head + payload
    rep.check(msg.hex() == doc["msg_hex"], "signed message MSG")
    rep.check(
        openssl_sign(seed, msg).hex() == doc["signature_hex"],
        "Ed25519 signature reproduced by OpenSSL",
    )
    rep.check(manifest[40:104].hex() == doc["signature_hex"], "signature placed at offset 40")
    rep.check(sha(manifest[:104] + payload).hex() == doc["manifest_id_hex"], "manifest_id")
    trusted = {key_id: pub}
    rep.check(read_envelope(manifest, trusted) == "ok", "valid manifest accepted")
    for case in doc["reject_cases"]:
        got = read_envelope(bytes.fromhex(case["manifest_hex"]), trusted)
        rep.check(
            got == case["expect"], f"reject/{case['label']}: expected {case['expect']}, got {got}"
        )
    probe = doc["payload_probe"]
    for case in probe["cases"]:
        blob = bytes.fromhex(case["manifest_hex"])
        rep.check(blob[104:].hex() == case["payload_hex"], f"probe/{case['label']}: payload bytes")
        rep.check(
            probe_min_reader(bytes.fromhex(case["payload_hex"])) == case["min_reader"],
            f"probe/{case['label']}: min_reader",
        )
        got = read_manifest_head(blob, trusted, probe["supported_payload_revision"])
        rep.check(
            got == case["expect"], f"probe/{case['label']}: expected {case['expect']}, got {got}"
        )


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    skip_zstd = "--skip-zstd" in sys.argv
    path = Path(args[0]) if args else HERE / "vectors-v0.json"
    doc = json.loads(path.read_text(encoding="utf-8"))
    rep = Report()
    verify_chunker(doc["chunker"], rep)
    verify_codecs(doc["codecs"], rep, skip_zstd)
    verify_envelope(doc["envelope"], rep)
    print(f"{rep.checks} checks, {len(rep.failures)} failures")
    for failure in rep.failures:
        print("FAIL:", failure)
    return 1 if rep.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
