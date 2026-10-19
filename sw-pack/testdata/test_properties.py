# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Seeded property / differential tests: generator implementation vs the independent one.

Run:  python test_properties.py -v     (standard library only, deterministic seeds)
"""

from __future__ import annotations

import hashlib
import random
import unittest

import gen_vectors as gen
import verify_vectors as ref

SPEC = (32 * 1024, 128 * 1024, 256 * 1024)
SMALL = (256, 1024, 4096)


def random_bytes(rng: random.Random, n: int, repeats: bool) -> bytes:
    if not repeats:
        return rng.randbytes(n)
    out = bytearray()
    while len(out) < n:
        if out and rng.random() < 0.5:
            start = rng.randrange(len(out))
            out += out[start : start + rng.randrange(4, 300)]
        else:
            out += rng.randbytes(rng.randrange(1, 40))
    return bytes(out[:n])


class ChunkerProperties(unittest.TestCase):
    def test_both_implementations_agree_and_obey_bounds(self) -> None:
        rng = random.Random(0xC0FFEE)
        for _ in range(150):
            n = rng.choice([0, 1, 255, 256, 257, 1024, 4096, 4097, rng.randrange(0, 70_000)])
            data = random_bytes(rng, n, repeats=rng.random() < 0.3)
            mn, avg, mx = SMALL
            a = gen.chunk_lengths(data, mn, avg, mx)
            b = ref.chunker_lengths(data, mn, avg, mx)
            self.assertEqual(a, b)
            self.assertEqual(sum(a), n)
            self.assertTrue(all(length <= mx for length in a))
            self.assertTrue(all(length >= mn for length in a[:-1]))

    def test_deterministic(self) -> None:
        data = random_bytes(random.Random(1), 50_000, repeats=False)
        self.assertEqual(gen.chunk_lengths(data, *SMALL), gen.chunk_lengths(data, *SMALL))

    def test_one_byte_insertion_changes_few_chunks(self) -> None:
        """F1-01 in miniature: one inserted byte alters at most 3 chunks (1 MiB, spec params)."""
        worst = 0
        for seed in range(8):
            rng = random.Random(seed)
            data = rng.randbytes(1 << 20)
            pos = rng.randrange(1, len(data) - 1)
            edited = data[:pos] + b"\x5a" + data[pos:]

            def digests(blob: bytes) -> list[str]:
                out, off = [], 0
                for length in gen.chunk_lengths(blob, *SPEC):
                    out.append(hashlib.sha256(blob[off : off + length]).hexdigest())
                    off += length
                return out

            before, after = digests(data), digests(edited)
            changed = len(set(after) - set(before))
            worst = max(worst, changed)
        self.assertLessEqual(worst, 3, f"worst case changed {worst} chunks")


class Lz4Properties(unittest.TestCase):
    def test_generator_output_decodes_in_independent_decoder(self) -> None:
        rng = random.Random(42)
        for _ in range(300):
            n = rng.choice([1, 2, 11, 12, 13, 16, 17, 100, rng.randrange(1, 5000)])
            raw = random_bytes(rng, n, repeats=rng.random() < 0.8)
            frame = gen.lz4_block_compress(raw)
            self.assertEqual(ref.lz4_block_decode(frame, n), raw)


class Ed25519Properties(unittest.TestCase):
    def test_pure_python_signature_matches_openssl_and_verifies(self) -> None:
        rng = random.Random(7)
        for _ in range(5):
            seed, msg = rng.randbytes(32), rng.randbytes(rng.randrange(0, 300))
            sig = gen.ed25519_sign(seed, msg)
            self.assertEqual(sig, ref.openssl_sign(seed, msg))
            pub = gen.ed25519_keys(seed)[2]
            self.assertEqual(pub, ref.openssl_pubkey(seed))
            self.assertTrue(ref.openssl_verify(pub, msg, sig))
            self.assertFalse(ref.openssl_verify(pub, msg + b"!", sig))


if __name__ == "__main__":
    unittest.main()
