# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Varre TODOS os arquivos do tag do libsodium atras de avisos de copyright/licenca (F1-04).

Le a arvore do tag pela API do GitHub (`gh api`, sem baixar tarball), obtem cada blob, e lista todo arquivo
que contem um marcador de licenca. Nao julga: imprime o achado, o componente e o escopo para uma pessoa
conferir. Uso:  python scan_libsodium_licenses.py 1.0.22-RELEASE CACHE_DIR > relatorio.md
O CACHE_DIR guarda os blobs baixados (fica fora do repositorio).
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = "jedisct1/libsodium"
MARKER = re.compile(
    r"copyright|licen[sc]e|permission is hereby|redistribution|public domain|\bCC0\b|\bGPL|\bBSD\b|\bISC\b|\bMIT\b",
    re.IGNORECASE,
)
# Escopo por prefixo de caminho (primeiro que casar). So a biblioteca entra no binario da libsodium.
SCOPES = [
    ("src/libsodium/", "biblioteca (compilada na libsodium)"),
    ("builds/", "build/CI (nao compilado)"),
    ("build-aux/", "build (gerado pelo autotools, nao compilado)"),
    ("dist-build/", "build (scripts, nao compilado)"),
    ("test/", "teste (nao compilado na biblioteca)"),
    ("m4/", "build (macros do autotools)"),
    ("contrib/", "contrib (nao compilado)"),
    ("msvc-scripts/", "build (nao compilado)"),
    ("libsodium-", "build (nao compilado)"),
]


def run_gh(*args: str) -> bytes:
    done = subprocess.run(["gh", "api", "--allow-escape-sequences", *args], capture_output=True, check=False)
    if done.returncode != 0:
        raise RuntimeError(done.stderr.decode(errors="replace"))
    return done.stdout


def scope_of(path: str) -> str:
    if path.endswith((".in", ".am", ".m4", ".sh")):
        return "build (gerado/script do autotools, nao compilado)"
    for prefix, name in SCOPES:
        if path.startswith(prefix):
            return name
    if "/" not in path:
        return "raiz do repositorio (documentacao/build, nao compilado)"
    return "outro (nao compilado)"


def component_of(path: str) -> str:
    parts = path.split("/")
    if path.startswith("src/libsodium/") and len(parts) >= 4:
        return "/".join(parts[2:4]) if parts[2].startswith("crypto_") else parts[2]
    return parts[0]


def fetch(tag: str, cache: Path) -> list[tuple[str, str, Path]]:
    tree = json.loads(run_gh(f"repos/{REPO}/git/trees/{tag}?recursive=1"))
    if tree.get("truncated"):
        raise RuntimeError("arvore truncada: a varredura nao cobriria tudo")
    blobs = [e for e in tree["tree"] if e["type"] == "blob"]

    def get(entry: dict[str, str]) -> tuple[str, str, Path]:
        dst = cache / entry["path"]
        if not dst.exists():
            raw = run_gh("-H", "Accept: application/vnd.github.raw", f"repos/{REPO}/git/blobs/{entry['sha']}")
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(raw)
        return entry["path"], entry["sha"], dst

    with ThreadPoolExecutor(8) as pool:
        return list(pool.map(get, blobs))


def main() -> int:
    tag, cache = sys.argv[1], Path(sys.argv[2])
    files = fetch(tag, cache)
    binary = 0
    hits: list[tuple[str, str, list[tuple[int, str]]]] = []
    for path, sha, local in files:
        data = local.read_bytes()
        if b"\0" in data[:8192]:
            binary += 1
            continue
        lines = data.decode("utf-8", errors="replace").splitlines()
        found = [(n + 1, text.strip()[:110]) for n, text in enumerate(lines) if MARKER.search(text)]
        if found:
            hits.append((path, sha, found))
    print(f"# Varredura de licencas do libsodium {tag}\n")
    print(f"- Arquivos no tag (blobs): {len(files)}; binarios ignorados (NUL nos 8 KiB iniciais): {binary}")
    print(f"- Arquivos de texto varridos: {len(files) - binary}")
    print(f"- Criterio: qualquer linha com marcador `{MARKER.pattern}` (sem distinguir maiusculas)")
    print(f"- Arquivos com ao menos um marcador: {len(hits)}\n")
    print("| Arquivo | Escopo | Componente | Blob | Linhas (n de marcadores; 1a) |")
    print("| --- | --- | --- | --- | --- |")
    for path, sha, found in sorted(hits):
        first = found[0][1].replace("|", "\\|")
        print(f"| {path} | {scope_of(path)} | {component_of(path)} | {sha[:10]} | {len(found)}; L{found[0][0]}: {first} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
