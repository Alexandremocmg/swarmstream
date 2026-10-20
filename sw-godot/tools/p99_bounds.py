# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alexandre Aparecido Rodrigues Peres
"""Contas de estatística de ordem do F1-48 (docs/08, "Método do percentil e amostras").

Sem supor distribuição nenhuma: o número de esperas abaixo do quantil verdadeiro q segue uma
Binomial(N, q). Daí saem o posto do p99 pelo posto mais próximo, quantas esperas ficam acima
dele e o posto da cota superior de 95% para o p99 verdadeiro (quando existe).

    python sw-godot/tools/p99_bounds.py 299 300 1000 2000
"""

from __future__ import annotations

import math
import sys


def _log_pmf(i: int, n: int, p: float) -> float:
    return (
        math.lgamma(n + 1)
        - math.lgamma(i + 1)
        - math.lgamma(n - i + 1)
        + i * math.log(p)
        + (n - i) * math.log1p(-p)
    )


def binom_cdf(k: int, n: int, p: float) -> float:
    """P(Binomial(n, p) <= k)."""
    if k < 0:
        return 0.0
    return min(1.0, sum(math.exp(_log_pmf(i, n, p)) for i in range(min(k, n) + 1)))


def nearest_rank(n: int, q: float = 0.99) -> int:
    """Posto (1-based) do percentil q pelo posto mais próximo: ceil(q * N)."""
    return max(math.ceil(q * n), 1)


def upper_bound_rank(n: int, q: float = 0.99, conf: float = 0.95) -> int | None:
    """Menor posto r com P(X_(r) >= quantil q verdadeiro) >= conf, ou None se não existe."""
    for r in range(1, n + 1):
        if binom_cdf(r - 1, n, q) >= conf:
            return r
    return None


def main(argv: list[str]) -> int:
    sizes = [int(a) for a in argv] or [100, 200, 299, 300, 600, 1000, 2000, 5000]
    print("N | posto do p99 | esperas acima | posto da cota de 95% | P(p99 amostral >= p99 real)")
    for n in sizes:
        k = nearest_rank(n)
        ub = upper_bound_rank(n)
        p_over = binom_cdf(k - 1, n, 0.99)
        shown = "não existe" if ub is None else f"{ub} (percentil {100 * ub / n:.2f})"
        print(f"{n} | {k} | {n - k} | {shown} | {p_over:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
