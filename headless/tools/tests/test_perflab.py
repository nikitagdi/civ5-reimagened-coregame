"""Minimal tests for civsim.perflab: python -m unittest discover -s headless\\tools\\tests -p "test_perflab*.py"."""
from __future__ import annotations

import math
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_simrunner import Sandbox  # noqa: E402

from civsim.perflab import __main__ as PM  # noqa: E402
from civsim.perflab import measure as M  # noqa: E402
from civsim.perflab import stats as ST  # noqa: E402
from civsim.simrunner import fakehost as F  # noqa: E402
from civsim.simrunner import scenario as S  # noqa: E402


class StatsTests(unittest.TestCase):
    def test_medians_ratio_bootstrap(self) -> None:
        runs = [{t: 10.0 + t for t in range(1, 41)}, {t: 12.0 + t for t in range(1, 41)}, {t: 11.0 + t for t in range(1, 41)}]
        med = ST.per_turn_medians(runs, window=(11, 40), skip_first=0)
        self.assertEqual((min(med), max(med), med[20]), (11, 40, 31.0))
        self.assertEqual(M.abba_order(2), ["A", "B", "B", "A"])
        rng = random.Random(1)
        # synthetic: B is exactly 2x faster than A with 3% noise -> geomean ~2, CI brackets 2 and is reproducible
        scn = []
        for _ in range(6):
            a = {t: 20.0 * (1 + rng.uniform(-0.03, 0.03)) for t in range(1, 51)}
            b = {t: 10.0 * (1 + rng.uniform(-0.03, 0.03)) for t in range(1, 51)}
            scn.append({"med_a": a, "med_b": b})
        s1 = ST.summarize(scn, iters=300)
        s2 = ST.summarize(scn, iters=300)
        self.assertEqual(s1["ci95"], s2["ci95"])                                     # fixed seed -> deterministic
        lo, hi = s1["ci95"]
        self.assertTrue(lo < s1["geomean"] < hi and lo < 2.0 < hi and math.isclose(s1["geomean"], 2.0, rel_tol=0.03), s1)

    def test_gate_rules(self) -> None:
        m = {"failed": [], "digest_mismatch": [], "geomean": 1.10, "ci95": [1.06, 1.14], "n_blocks": 20, "ci_halfwidth_rel": 0.036}
        self.assertEqual(PM.evaluate_gate(m, "confirmed")[0], 4)                      # slower by > 3% and CI lower bound > 1
        self.assertEqual(PM.evaluate_gate({**m, "geomean": 0.98, "ci95": [0.97, 0.99], "ci_halfwidth_rel": 0.01}, "confirmed")[0], 0)
        ok = {**m, "geomean": 0.98, "ci95": [0.97, 0.99], "ci_halfwidth_rel": 0.01}
        self.assertEqual(PM.evaluate_gate(ok, "missing")[0], 5)                       # equivalence not confirmed
        self.assertEqual(PM.evaluate_gate(ok, "skipped")[0], 0)
        self.assertEqual(PM.evaluate_gate({**ok, "ci_halfwidth_rel": 0.08}, "confirmed")[0], 5)   # noise above the MDE
        self.assertEqual(PM.evaluate_gate({**ok, "failed": ["x"]}, "confirmed")[0], 1)


class MeasureTests(Sandbox):
    def test_measure_fake_host_ratio_and_gate_inputs(self) -> None:
        items = [S.Item(f"s{i}", F.sample_scenario(f"s{i}", seed=i, turns=24), "") for i in (1, 2)]
        r = M.measure(self.ctx, items, self.dll["good"], self.dll["fast"], repeat=2, out_dir=self.root / "perf1", bootstrap_iters=200)
        self.assertFalse(r["failed"] or r["digest_mismatch"])
        self.assertTrue(math.isclose(r["geomean"], 2.0, rel_tol=0.05), r["geomean"])                # the fake DLLs differ 2x in ms/turn
        self.assertTrue((self.root / "perf1" / "measure.json").is_file())
        aa = M.measure(self.ctx, items, self.dll["good"], self.dll["good"], repeat=2, out_dir=self.root / "perf2", bootstrap_iters=200)
        self.assertTrue(math.isclose(aa["geomean"], 1.0, rel_tol=0.05), aa["geomean"])              # A/A is ~1
        bad = M.measure(self.ctx, items[:1], self.dll["good"], self.dll["bad"], repeat=1, out_dir=self.root / "perf3", bootstrap_iters=50)
        self.assertEqual(bad["digest_mismatch"], ["s1"])                                            # different work is flagged


if __name__ == "__main__":
    unittest.main()
