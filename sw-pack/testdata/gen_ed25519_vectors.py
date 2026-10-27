#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Gera ed25519-vectors.txt com o OpenSSL (via `cryptography`) para testar a integração com o
libsodium (F1-04, ADR-0012). Determinístico: sementes e mensagens vêm de SHA-256.

Cada linha: semente, chave pública, mensagem e assinatura, em hexadecimal ("-" = mensagem vazia).

Uso:  python -I gen_ed25519_vectors.py > ed25519-vectors.txt
(precisa de `pip install cryptography`)
"""

import hashlib

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

CASES = 96


def stream(label: str, n: int) -> bytes:
    out = b""
    counter = 0
    while len(out) < n:
        out += hashlib.sha256(f"swarmstream-ed25519-v0|{label}|{counter}".encode()).digest()
        counter += 1
    return out[:n]


def main() -> None:
    for i in range(CASES):
        seed = stream(f"seed{i}", 32)
        length = (0, 1, 31, 32, 33, 63, 64, 65, 127, 128, 129)[i] if i < 11 else (i * 37) % 700
        message = stream(f"msg{i}", length)
        key = Ed25519PrivateKey.from_private_bytes(seed)
        public_key = key.public_key().public_bytes_raw()
        signature = key.sign(message)
        print(seed.hex(), public_key.hex(), message.hex() or "-", signature.hex())


if __name__ == "__main__":
    main()
