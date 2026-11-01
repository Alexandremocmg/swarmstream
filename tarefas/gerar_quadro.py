# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Gera tarefas/QUADRO.md a partir dos cartões em tarefas/cartoes/.

Uso: python tarefas/gerar_quadro.py [--check]
  --check  não escreve; sai com código 1 se o QUADRO.md estiver desatualizado.
Só biblioteca padrão. Quem mantém o QUADRO.md é a gestão; os agentes editam apenas o seu cartão.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ESTADOS = ["EM_ANDAMENTO", "EM_REVISAO", "AGUARDANDO_DONO", "BLOQUEADA", "ABERTA", "FEITA"]
FIELD = re.compile(r"^- \*\*(?P<k>[^:]+):\*\* (?P<v>.*)$")


def ler_cartao(path: Path) -> dict[str, str]:
    dados: dict[str, str] = {"id": path.stem}
    linhas = path.read_text(encoding="utf-8").splitlines()
    dados["titulo"] = linhas[0].lstrip("# ").split(": ", 1)[-1] if linhas else path.stem
    for linha in linhas:
        m = FIELD.match(linha)
        if m and m["k"] not in dados:
            dados[m["k"]] = m["v"].strip()
    return dados


def faltando(cartao: dict[str, str]) -> list[str]:
    obrigatorios = ["Estado", "Fase", "Papel sugerido", "Sessão", "Depende de"]
    return [c for c in obrigatorios if c not in cartao]


def dependencias(cartao: dict[str, str]) -> list[str]:
    # sem repetição: o link [F1-63](F1-63.md) casa duas vezes, e um ID citado de novo no texto também
    ids = re.findall(r"F1-\d+", cartao.get("Depende de", "").split("|")[0])
    return list(dict.fromkeys(ids))


def montar(cartoes: list[dict[str, str]]) -> str:
    por_id = {c["id"]: c for c in cartoes}
    erros: list[str] = []
    for c in cartoes:
        if c.get("Estado") not in ESTADOS:
            erros.append(f"{c['id']}: estado inválido {c.get('Estado')!r}")
        if faltando(c):
            erros.append(f"{c['id']}: campos ausentes {faltando(c)}")
    if erros:
        raise SystemExit("Cartões inválidos:\n  " + "\n  ".join(erros))

    def pronta(c: dict[str, str]) -> bool:
        return c["Estado"] == "ABERTA" and all(
            por_id[d]["Estado"] == "FEITA" for d in dependencias(c) if d in por_id
        )

    saida = ["# Quadro de tarefas\n"]
    saida.append(
        "Gerado por `python tarefas/gerar_quadro.py` a partir dos cartões. **Não edite à mão.** "
        "Como funciona: [README.md](README.md). Decisões: [decisoes.md](decisoes.md).\n"
    )
    contagem = {e: sum(1 for c in cartoes if c["Estado"] == e) for e in ESTADOS}
    saida.append("| " + " | ".join(ESTADOS) + " |")
    saida.append("| " + " | ".join("---" for _ in ESTADOS) + " |")
    saida.append("| " + " | ".join(str(contagem[e]) for e in ESTADOS) + " |\n")

    aguardando = [c for c in cartoes if c["Estado"] == "AGUARDANDO_DONO"]
    if aguardando:
        saida.append("## Espera o dono do produto")
        for c in aguardando:
            saida.append(f"- [{c['id']}](cartoes/{c['id']}.md): {c['titulo']}")
        saida.append("")

    for estado in ["EM_ANDAMENTO", "EM_REVISAO", "BLOQUEADA"]:
        itens = [c for c in cartoes if c["Estado"] == estado]
        if itens:
            saida.append(f"## {estado}")
            for c in itens:
                saida.append(
                    f"- [{c['id']}](cartoes/{c['id']}.md) ({c['Papel sugerido']}, sessão: {c['Sessão']}): {c['titulo']}"
                )
            saida.append("")

    for fase in ["F1a", "F1b"]:
        abertas = [c for c in cartoes if c["Fase"] == fase and c["Estado"] == "ABERTA"]
        if not abertas:
            continue
        saida.append(f"## Abertas na {fase}")
        saida.append("| Cartão | Papel | Modelo e esforço | Crítico | Pode começar? | Tarefa |")
        saida.append("| --- | --- | --- | --- | --- | --- |")
        for c in abertas:
            pode = "sim" if pronta(c) else "espera " + ", ".join(
                d for d in dependencias(c) if d in por_id and por_id[d]["Estado"] != "FEITA"
            )
            critico = "sim" if c.get("Código crítico", "").startswith("sim") else "não"
            saida.append(
                f"| [{c['id']}](cartoes/{c['id']}.md) | {c['Papel sugerido']} | "
                f"{c.get('Modelo e esforço sugeridos', '')} | {critico} | {pode} | {c['titulo']} |"
            )
        saida.append("")

    feitas = [c for c in cartoes if c["Estado"] == "FEITA"]
    if feitas:
        saida.append("## Feitas")
        for c in feitas:
            saida.append(f"- [{c['id']}](cartoes/{c['id']}.md): {c['titulo']}")
        saida.append("")
    return "\n".join(saida).rstrip() + "\n"


def main() -> int:
    base = Path(__file__).resolve().parent
    cartoes = [ler_cartao(p) for p in sorted((base / "cartoes").glob("*.md"))]
    texto = montar(cartoes)
    alvo = base / "QUADRO.md"
    if "--check" in sys.argv:
        atual = alvo.read_text(encoding="utf-8") if alvo.exists() else ""
        if atual.replace("\r\n", "\n") != texto:
            print("tarefas/QUADRO.md está desatualizado: rode python tarefas/gerar_quadro.py")
            return 1
        print("QUADRO.md em dia")
        return 0
    alvo.write_text(texto, encoding="utf-8", newline="\n")
    print(f"QUADRO.md gerado com {len(cartoes)} cartões")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
