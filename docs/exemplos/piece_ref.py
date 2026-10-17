"""Referência mínima do hash de peça do Swarmstream (spec 03, v0).

Uso: python -I piece_ref.py   -> imprime os vetores de teste da seção 'Vetores' de docs/03.
Só biblioteca padrão. Serve para conferir implementações em C++; não é código de produção.
"""
import hashlib

LEAF = 16 * 1024          # folha Merkle
PIECE_MAX = 1024 * 1024   # peça: tamanho máximo
LEAVES_PER_PIECE = PIECE_MAX // LEAF  # 64
ZERO = bytes(32)


def sha256(b: bytes) -> bytes:
    return hashlib.sha256(b).digest()


def piece_root(piece: bytes) -> bytes:
    """Raiz Merkle de uma peça: 64 folhas; folha ausente = 32 bytes zero."""
    assert 0 < len(piece) <= PIECE_MAX
    level = [sha256(piece[i:i + LEAF]) for i in range(0, len(piece), LEAF)]
    level += [ZERO] * (LEAVES_PER_PIECE - len(level))
    while len(level) > 1:
        level = [sha256(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
    return level[0]


def make_block(n: int, seed: int) -> bytes:
    return bytes((i * 7 + seed) % 256 for i in range(n))


if __name__ == "__main__":
    raws = [make_block(1000, 1), make_block(20000, 2), make_block(5000, 3)]  # codec 0 (none)
    piece = b"".join(raws)
    off = 0
    for i, r in enumerate(raws):
        print(f"bloco {i}: raw={len(r)} packed={len(r)} offset_in_piece={off} sha256={sha256(r).hex()}")
        off += len(r)
    print("peça: size =", len(piece))
    for i in range(0, len(piece), LEAF):
        print(f"  folha {i // LEAF}: len={len(piece[i:i+LEAF])} sha256={sha256(piece[i:i+LEAF]).hex()}")
    print("piece_hash (raiz Merkle) =", piece_root(piece).hex())
    print("peça de 1 byte 0x00, raiz =", piece_root(b"\x00").hex())
