# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Varre TODOS os arquivos do tag do libsodium atras de avisos de copyright/licenca (F1-04).

Le a arvore do tag pela API do GitHub (`gh api`, sem baixar tarball) e cada blob. Tres modos:

  scan_libsodium_licenses.py TAG CACHE                 relatorio (Markdown) de todos os achados
  scan_libsodium_licenses.py TAG CACHE --write DIR     regenera os 3 arquivos de avisos em DIR
  scan_libsodium_licenses.py TAG CACHE --verify DIR    confere que cada bloco dos 3 arquivos aparece
                                                       LITERALMENTE no blob de cada arquivo listado,
                                                       com o SHA do blob certo, e que nenhum arquivo
                                                       compilado com aviso ficou de fora

O arquivo de avisos e gerado (`--write`) e conferido (`--verify`) pela mesma regra: o bloco de cada
arquivo e o comentario do inicio que contem copyright/domain/CC0; arquivos com o mesmo bloco
(byte a byte) formam um grupo. CACHE guarda os blobs baixados (fica fora do repositorio).
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = "jedisct1/libsodium"
LIB = "src/libsodium/"
MARKER = re.compile(
    r"copyright|licen[sc]e|permission is hereby|redistribution|public domain|\bCC0\b|\bGPL"
    r"|\bBSD\b|\bISC\b|\bMIT\b",
    re.IGNORECASE,
)
BLOCK_MARKER = re.compile(r"copyright|public domain|CC0", re.IGNORECASE)
BUILD_SUFFIXES = (".in", ".am", ".m4", ".sh")
# Escopo por prefixo de caminho (o primeiro que casar).
SCOPES = [
    (LIB, "biblioteca (compilada na libsodium)"),
    ("builds/", "build/CI (nao compilado)"),
    ("build-aux/", "build (gerado pelo autotools, nao entra no .a)"),
    ("dist-build/", "build (scripts, nao entra no .a)"),
    ("test/", "teste (nao entra no .a)"),
    ("m4/", "build (macros do autotools, nao entra no .a)"),
    ("contrib/", "contrib (nao compilado)"),
    ("msvc-scripts/", "build (nao compilado)"),
]
NOTICE_FILES = {
    "percival": "libsodium-1.0.22-BSD-Percival.txt",
    "peslyak": "libsodium-1.0.22-BSD-Peslyak-scrypt.txt",
    "publico": "libsodium-1.0.22-dominio-publico-CC0.txt",
}
TITLES = {
    "percival": "Colin Percival: SHA-256/SHA-512 de referencia e PBKDF2-SHA256 (BSD-2-Clause).",
    "peslyak": "scrypt: avisos de Alexander Peslyak (BSD-style) e arquivos com Percival + Peslyak.",
    "publico": "Dominio publico / CC0: D. J. Bernstein, BLAKE2 (Samuel Neves), Argon2 (Dinu e "
    "Khovratovich), Thomas Pornin.",
}
HEADER = (
    "Cada bloco abaixo e o comentario inicial, copiado SEM alteracao, dos arquivos listados logo\n"
    "acima dele (caminho e SHA-1 do blob git no tag 1.0.22-RELEASE de\n"
    "https://github.com/jedisct1/libsodium, lido por `gh api`, sem tarball). Arquivos com blocos\n"
    "diferentes ficam em grupos diferentes. Regenerar: `scan_libsodium_licenses.py TAG CACHE\n"
    "--write DIR`; conferir: `--verify DIR` (cada bloco tem de estar literalmente em cada blob).\n"
    "Varredura completa: docs/medicoes/varredura-licencas-libsodium-1.0.22.md\n"
)
RULE = "=" * 78


def run_gh(*args: str) -> bytes:
    cmd = ["gh", "api", "--allow-escape-sequences", *args]
    done = subprocess.run(cmd, capture_output=True, check=False)  # noqa: S603 (gh, args fixos)
    if done.returncode != 0:
        raise RuntimeError(done.stderr.decode(errors="replace"))
    return done.stdout


def scope_of(path: str) -> str:
    if path.endswith(BUILD_SUFFIXES):
        return "build (gerado/script do autotools, nao entra no .a)"
    for prefix, name in SCOPES:
        if path.startswith(prefix):
            return name
    if "/" not in path:
        return "raiz do repositorio (documentacao/build, nao entra no .a)"
    return "outro (nao compilado)"


def component_of(path: str) -> str:
    parts = path.split("/")
    if path.startswith(LIB) and len(parts) >= 4:
        return "/".join(parts[2:4]) if parts[2].startswith("crypto_") else parts[2]
    return parts[0]


def fetch(tag: str, cache: Path) -> dict[str, tuple[str, Path]]:
    """Devolve {caminho: (sha do blob, arquivo local)} para TODOS os blobs do tag."""
    tree = json.loads(run_gh(f"repos/{REPO}/git/trees/{tag}?recursive=1"))
    if tree.get("truncated"):
        raise RuntimeError("arvore truncada: a varredura nao cobriria tudo")
    blobs = [e for e in tree["tree"] if e["type"] == "blob"]

    def get(entry: dict[str, str]) -> tuple[str, tuple[str, Path]]:
        dst = cache / entry["path"]
        if not dst.exists():
            accept = ["-H", "Accept: application/vnd.github.raw"]
            raw = run_gh(*accept, f"repos/{REPO}/git/blobs/{entry['sha']}")
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(raw)
        return entry["path"], (entry["sha"], dst)

    with ThreadPoolExecutor(8) as pool:
        return dict(pool.map(get, blobs))


def read_text(local: Path) -> str | None:
    data = local.read_bytes()
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", errors="replace")


def license_block(text: str) -> str | None:
    """Comentario `/* ... */` do inicio que contem copyright/domain/CC0 (3000 bytes iniciais)."""
    found = BLOCK_MARKER.search(text[:3000])
    if not found:
        return None
    start = text.rfind("/*", 0, found.start())
    end = text.find("*/", found.start())
    if start == -1 or end == -1:
        return None
    return text[start : end + 2]


def family_of(block: str) -> str:
    if "Peslyak" in block:
        return "peslyak"
    if "Percival" in block:
        return "percival"
    return "publico"


def compiled_blocks(files: dict[str, tuple[str, Path]]) -> dict[str, list[tuple[str, list[str]]]]:
    """{familia: [(bloco, [caminhos])...]} so dos arquivos da biblioteca (nao build)."""
    groups: dict[str, dict[str, list[str]]] = {}
    for path, (_sha, local) in sorted(files.items()):
        if not path.startswith(LIB) or path.endswith(BUILD_SUFFIXES):
            continue
        text = read_text(local)
        block = license_block(text) if text is not None else None
        if block is not None:
            groups.setdefault(family_of(block), {}).setdefault(block, []).append(path)
    return {fam: [(blk, paths) for blk, paths in g.items()] for fam, g in groups.items()}


def render(
    family: str, groups: list[tuple[str, list[str]]], files: dict[str, tuple[str, Path]]
) -> str:
    out = [TITLES[family], "", HEADER]
    for block, paths in groups:
        out += [RULE, "Arquivos (caminho  blob SHA-1):"]
        out += [f"  {p}  {files[p][0]}" for p in paths]
        out += ["", block, ""]
    return "\n".join(out)


def write_notices(files: dict[str, tuple[str, Path]], dest: Path) -> None:
    by_family = compiled_blocks(files)
    for family, name in NOTICE_FILES.items():
        (dest / name).write_text(
            render(family, by_family[family], files), encoding="utf-8", newline="\n"
        )


def verify_notices(files: dict[str, tuple[str, Path]], dest: Path) -> int:
    """Confere cada arquivo de avisos contra os blobs do tag; devolve o numero de erros."""
    errors = 0
    listed: set[str] = set()
    for name in NOTICE_FILES.values():
        text = (dest / name).read_text(encoding="utf-8")
        for section in text.split(RULE + "\n")[1:]:
            head, _, rest = section.partition("\n\n")
            block = rest.rstrip("\n")
            for line in head.splitlines()[1:]:
                path, sha = line.split()
                listed.add(path)
                blob = read_text(files[path][1]) if path in files else None
                if path not in files or files[path][0] != sha:
                    print(f"ERRO {name}: {path} com SHA {sha} nao e o blob do tag")
                    errors += 1
                elif blob is None or block not in blob:
                    print(f"ERRO {name}: o bloco nao aparece literalmente em {path}")
                    errors += 1
    expected = {p for groups in compiled_blocks(files).values() for _b, ps in groups for p in ps}
    for path in sorted(expected - listed):
        print(f"ERRO: {path} tem aviso e nao esta em nenhum arquivo de avisos")
        errors += 1
    for path in sorted(listed - expected):
        print(f"ERRO: {path} listado, mas sem aviso na varredura")
        errors += 1
    print(
        f"verificacao: {len(listed)} arquivos listados, {len(expected)} esperados, {errors} erros"
    )
    return errors


def report(tag: str, files: dict[str, tuple[str, Path]]) -> None:
    binary = 0
    hits: list[tuple[str, str, list[tuple[int, str]]]] = []
    for path, (sha, local) in files.items():
        text = read_text(local)
        if text is None:
            binary += 1
            continue
        found = [
            (n + 1, t.strip()[:110]) for n, t in enumerate(text.splitlines()) if MARKER.search(t)
        ]
        if found:
            hits.append((path, sha, found))
    print(f"- Tag: {tag}; blobs no tag: {len(files)}; binarios ignorados: {binary}")
    print(f"- Arquivos de texto varridos: {len(files) - binary}")
    print(f"- Criterio: qualquer linha com `{MARKER.pattern}` (sem distinguir maiusculas)")
    print(f"- Arquivos com ao menos um marcador: {len(hits)}\n")
    print("| Arquivo | Escopo | Componente | Blob | Linhas (n de marcadores; 1a) |")
    print("| --- | --- | --- | --- | --- |")
    for path, sha, found in sorted(hits):
        first = found[0][1].replace("|", "\\|")
        count, line = len(found), found[0][0]
        print(
            f"| {path} | {scope_of(path)} | {component_of(path)} | {sha[:10]} | "
            f"{count}; L{line}: {first} |"
        )


def main(argv: list[str]) -> int:
    tag, cache = argv[1], Path(argv[2])
    files = fetch(tag, cache)
    if len(argv) == 5 and argv[3] == "--write":
        write_notices(files, Path(argv[4]))
        return 0
    if len(argv) == 5 and argv[3] == "--verify":
        return 1 if verify_notices(files, Path(argv[4])) else 0
    report(tag, files)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
