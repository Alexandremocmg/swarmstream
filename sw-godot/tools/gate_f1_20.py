# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Roda o portão obrigatório do ADR-0016 (F1-20) do zero e devolve 0 ou 1.

Etapas: (1) gera o áudio OGG com o ffmpeg; (2) empacota textura e malha com
scripts/gate_pack.gd; (3) importa o projeto (registra a GDExtension);
(4) roda a cena scenes/gate_f1_20.tscn com janela (GPU e áudio de verdade) e lê
o resultado. A GDExtension precisa estar compilada antes (sw-godot/README.md).

    python sw-godot/tools/gate_f1_20.py --godot <godot_console.exe> [--out <pasta>]
"""

from __future__ import annotations

import argparse
import shutil
import subprocess  # noqa: S404 - roda Godot e ffmpeg locais, com argumentos fixos
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1] / "project"
PACK_DIR = PROJECT / ".swarm_gate"
LIB_NAME = "swarmstream_godot.dll" if sys.platform == "win32" else "swarmstream_godot.so"
DLL = PROJECT / "addons" / "swarmstream" / "bin" / LIB_NAME
GDEXT = PROJECT / "addons" / "swarmstream" / "swarmstream.gdextension"
RESULT_TAG = "[gate] RESULTADO:"


def run(cmd: list[str], timeout_s: float) -> tuple[int, str]:
    """Roda um comando e devolve (código, saída). Timeout conta como falha."""
    try:
        done = subprocess.run(  # noqa: S603 - comando montado só com caminhos locais
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout if isinstance(exc.stdout, str) else ""
        return 124, out + f"\n[gate_f1_20.py] TIMEOUT depois de {timeout_s:.0f} s"
    return done.returncode, done.stdout + done.stderr


def make_audio(ffmpeg: str) -> bool:
    target = PACK_DIR / "tone.ogg"
    if target.exists() and target.stat().st_size > 0:
        return True
    PACK_DIR.mkdir(parents=True, exist_ok=True)
    code, out = run(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=440:duration=3",
            "-c:a",
            "libvorbis",
            "-q:a",
            "4",
            str(target),
        ],
        60,
    )
    if code != 0:
        print(out, file=sys.stderr)
    return code == 0 and target.exists()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True, help="Godot 4.7.2 *_console.exe")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--out", type=Path, help="pasta para as capturas de tela e o log")
    parser.add_argument(
        "--headless",
        action="store_true",
        help="sem janela (renderizador e áudio falsos); não prova o envio BC1 à GPU",
    )
    args = parser.parse_args()

    if not DLL.exists() or not GDEXT.exists():
        print(f"GDExtension não compilada: {DLL} (ver sw-godot/README.md)", file=sys.stderr)
        return 1
    ffmpeg = shutil.which(args.ffmpeg) or args.ffmpeg
    if not make_audio(ffmpeg):
        print("falha ao gerar tone.ogg com o ffmpeg", file=sys.stderr)
        return 1
    godot = str(args.godot)
    steps: list[tuple[str, list[str], float]] = [
        (
            "pack",
            [godot, "--headless", "--path", str(PROJECT), "--script", "res://scripts/gate_pack.gd"],
            600,
        ),
        ("import", [godot, "--headless", "--path", str(PROJECT), "--import"], 900),
    ]
    for name, cmd, timeout_s in steps:
        code, out = run(cmd, timeout_s)
        lines = [line for line in out.splitlines() if line.startswith("PACK") or "ERROR" in line]
        print(f"== {name}: código {code}")
        print("\n".join(lines[-20:]))
        if code != 0:
            print(out[-4000:], file=sys.stderr)
            return 1

    cmd = [godot, *(["--headless"] if args.headless else []), "--path", str(PROJECT)]
    cmd += ["res://scenes/gate_f1_20.tscn", "--", "--gate-auto"]
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        cmd += ["--out", str(args.out.resolve())]
    code, out = run(cmd, 120)
    gate_lines = [line for line in out.splitlines() if line.startswith("[gate]") or "ERROR" in line]
    print("== gate: código", code)
    print("\n".join(gate_lines))
    if args.out:
        (args.out / "f1_20_gate.log").write_text(out, encoding="utf-8")
    if not any(line.startswith(RESULT_TAG) for line in gate_lines):
        print("a cena não imprimiu o resultado (extensão não carregou ou o script não compilou)")
        return 1
    return 0 if code == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
