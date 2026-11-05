"""Perf statistics: per-turn medians over K repeats, ratio over a window, geometric mean over
scenarios, 95% CI by block bootstrap (10-turn blocks inside a scenario, resample scenarios, 2000 iterations, random.Random(20261017))."""
from __future__ import annotations

import math
import random
import statistics

SEED = 20261017


def per_turn_medians(runs: list[dict[int, float]], window: tuple[int, int] | None = None, skip_first: int = 0) -> dict[int, float]:
    """runs: K dicts turn -> ms. Median over K per turn, turns present in every run, inside the window."""
    if not runs:
        return {}
    turns = sorted(set.intersection(*(set(r) for r in runs)))[skip_first:]
    if window:
        turns = [t for t in turns if window[0] <= t <= window[1]]
    return {t: statistics.median(r[t] for r in runs) for t in turns}


def ratio(med_a: dict[int, float], med_b: dict[int, float]) -> float | None:
    """sum med(A) / sum med(B) over the turns both have."""
    ts = sorted(set(med_a) & set(med_b))
    sb = sum(med_b[t] for t in ts)
    return sum(med_a[t] for t in ts) / sb if ts and sb > 0 else None


def geomean(xs: list[float]) -> float:
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def _blocks(a: list[float], b: list[float], size: int) -> list[tuple[float, float]]:
    return [(sum(a[i:i + size]), sum(b[i:i + size])) for i in range(0, len(a), size)]


def bootstrap_ci(scenarios: list[tuple[list[float], list[float]]], block: int = 10, iters: int = 2000, seed: int = SEED,
                 alpha: float = 0.05) -> tuple[float, float] | None:
    """scenarios: per scenario (med_A per turn, med_B per turn), aligned lists. Returns the CI of the geometric mean of sum(A)/sum(B)."""
    blocks = [_blocks(a, b, block) for a, b in scenarios if a]
    if not blocks:
        return None
    rng = random.Random(seed)
    s = len(blocks)
    stats = []
    for _ in range(iters):
        logs = []
        for _s in range(s):
            bl = blocks[rng.randrange(s)]
            n = len(bl)
            ka = kb = 0.0
            for _b in range(n):
                x, y = bl[rng.randrange(n)]
                ka += x
                kb += y
            if ka > 0 and kb > 0:
                logs.append(math.log(ka / kb))
        if logs:
            stats.append(math.exp(sum(logs) / len(logs)))
    if not stats:
        return None
    stats.sort()
    lo = stats[int(alpha / 2 * len(stats))]
    hi = stats[min(len(stats) - 1, math.ceil((1 - alpha / 2) * len(stats)) - 1)]
    return lo, hi


def summarize(per_scenario: list[dict], block: int = 10, iters: int = 2000) -> dict:
    """per_scenario: {"med_a": {turn: ms}, "med_b": {...}} -> geomean/min/max of r_s and the bootstrap CI."""
    rs, aligned = [], []
    for sc in per_scenario:
        ts = sorted(set(sc["med_a"]) & set(sc["med_b"]))
        r = ratio(sc["med_a"], sc["med_b"])
        sc["r"] = r
        if r:
            rs.append(r)
            aligned.append(([sc["med_a"][t] for t in ts], [sc["med_b"][t] for t in ts]))
    if not rs:
        return {"geomean": None, "min": None, "max": None, "ci95": None, "n_scenarios": 0, "n_blocks": 0}
    ci = bootstrap_ci(aligned, block, iters)
    return {"geomean": geomean(rs), "min": min(rs), "max": max(rs), "ci95": list(ci) if ci else None, "n_scenarios": len(rs),
            "n_blocks": sum(-(-len(a) // block) for a, _b in aligned)}
