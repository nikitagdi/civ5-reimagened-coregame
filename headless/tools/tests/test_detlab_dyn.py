"""Minimal tests for civsim.detlab dynamic commands (fake host): python -m unittest discover -s headless\\tools\\tests -p "test_detlab_dyn*.py"."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_simrunner import Sandbox  # noqa: E402

from civsim import digestbin as D  # noqa: E402
from civsim.detlab import dyn  # noqa: E402
from civsim.simrunner import fakehost as F  # noqa: E402
from civsim.simrunner import scenario as S  # noqa: E402


class DynTests(Sandbox):
    def setUp(self) -> None:
        super().setUp()
        self.item = S.Item("fake", F.sample_scenario("fake", seed=5, turns=8), "")

    def test_digest_compare_and_bisect_localize_the_divergence(self) -> None:
        a = D.build(["game", "player00"], [(1, [1, 2]), (2, [1, 3])])
        b = D.build(["game", "player00"], [(1, [1, 2]), (2, [9, 3])])
        c = D.compare(D.parse(a), D.parse(b))
        self.assertEqual((c["first_divergent_turn"], c["divergent_segments"], c["identical"]), (2, ["game"], False))
        self.assertTrue(D.compare(D.parse(a), D.parse(a))["identical"])

        r = dyn.bisect(self.ctx, self.item, self.dll["good"], self.dll["bad"], None, self.root / "bis")
        self.assertEqual((r["status"], r["first_divergent_turn"], r["divergent_segments"]), ("divergent", 4, ["player01"]))
        seg = r["segments"][0]
        self.assertEqual((seg["name"], seg["first_diff_offset"]), ("player01", 100))     # the fake flips byte 100 of that segment
        self.assertTrue(r["reproduced_on_rerun"] and not r["dump_perturbs_digest"])
        self.assertIn("player01", dyn.render_bisect_md(r))
        same = dyn.bisect(self.ctx, self.item, self.dll["good"], self.dll["fast"], None, self.root / "bis2")
        self.assertEqual(same["status"], "equal")

    def test_equiv_saveload_and_perturb(self) -> None:
        e = dyn.equiv(self.ctx, [self.item], self.dll["good"], self.dll["fast"], None, 1)
        self.assertTrue(e["all_equal"])
        e = dyn.equiv(self.ctx, [self.item], self.dll["good"], self.dll["bad"], None, 1)
        self.assertEqual(e["diverged"], ["fake"])

        ok = dyn.saveload(self.ctx, self.item, self.dll["good"], 3, 7, self.root / "sl1")
        self.assertEqual(ok["status"], "equal")
        bad = dyn.saveload(self.ctx, self.item, self.dll["nonser"], 3, 7, self.root / "sl2")      # non-serialized state: class C1
        self.assertEqual((bad["status"], bad["first_divergent_turn"], bad["divergent_segments"]), ("divergent", 4, ["player00"]))

        axes = [("active_player", ["0", "1"])]
        self.assertEqual(dyn.perturb(self.ctx, self.item, self.dll["good"], axes, None, 1)["sensitive"], [])
        sens = dyn.perturb(self.ctx, self.item, self.dll["c5"], axes, None, 1)
        self.assertEqual(sens["sensitive"], ["active_player"])
        self.assertEqual(sens["axes"][0]["class"], "C5")
        unsupported = dyn.perturb(self.ctx, self.item, self.dll["good"], [("fpu_cw", ["a", "b"])], None, 1)
        self.assertEqual(unsupported["axes"][0]["status"], "unsupported")                          # the fake host has no --fpu-cw


if __name__ == "__main__":
    unittest.main()
