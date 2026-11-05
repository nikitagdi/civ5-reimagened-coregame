"""Minimal tests for civsim.simrunner (fake host): python -m unittest discover -s headless\\tools\\tests -p "test_simrunner*.py"."""
from __future__ import annotations

import copy
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from civsim import common as C  # noqa: E402
from civsim import digestbin  # noqa: E402
from civsim.simrunner import fakehost as F  # noqa: E402
from civsim.simrunner import runner as RN  # noqa: E402
from civsim.simrunner import scenario as S  # noqa: E402


class Sandbox(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory(prefix="simrunner-test-")
        self.addCleanup(self._td.cleanup)
        self.root = Path(self._td.name)
        self.sb = F.make_sandbox(self.root)
        C.use_hash_memo(self.root / "hashcache.json")
        self.addCleanup(C.use_hash_memo, None)
        self.ctx = RN.Ctx(host=self.sb["host"], data=self.sb["data"], runs_root=self.sb["runs_root"], batches_root=self.sb["batches_root"])
        self.dll = {k: C.resolve_dll(v) for k, v in self.sb["dlls"].items()}


class ScenarioTests(unittest.TestCase):
    def test_key_invariance_validation_and_corpora(self) -> None:
        scn = F.sample_scenario()
        key = lambda s: S.sim_key(S.canonical(s, 8), "d" * 64, "e" * 64, "f" * 64)  # noqa: E731
        reordered = {k: scn[k] for k in reversed(list(scn))}
        reordered.update(name="other name", note="free text", outputs={"digest": "per-turn"})
        self.assertEqual(key(scn), key(reordered))                  # key order, name, note, outputs do not matter
        changed = copy.deepcopy(scn)
        changed["seeds"]["map"] += 1
        self.assertNotEqual(key(scn), key(changed))
        self.assertNotEqual(S.sim_key(S.canonical(scn, 8), "a" * 64, "e" * 64, "f" * 64), key(scn))   # DLL hash is part of the key

        bad = copy.deepcopy(scn)
        del bad["seeds"]["sync"]
        bad["max_turns"] = 1.5
        errs, _ = S.validate(bad)
        self.assertTrue(any("seeds.sync" in e for e in errs) and any("float" in e or "max_turns" in e for e in errs), errs)
        self.assertEqual(S.validate(scn)[0], [])

        starter = S.expand_corpus(C.CORPORA / "starter.json")
        realistic = S.expand_corpus(C.CORPORA / "realistic.json")
        self.assertEqual(len(starter), 3)
        self.assertEqual(len(realistic), 12)
        self.assertEqual([i.name for i in realistic], [i.name for i in S.expand_corpus(C.CORPORA / "realistic.json")])
        self.assertEqual({len(i.scenario["civs"]) for i in realistic}, {4, 6, 8})
        for it in starter + realistic + S.expand_corpus(C.CORPORA / "stress.json") + S.expand_corpus(C.CORPORA / "early.json"):
            self.assertEqual(S.validate(it.scenario)[0], [], it.name)


class RunTests(Sandbox):
    def test_batch_repeat_cache_and_artifacts(self) -> None:
        items = [S.Item(f"s{i}", F.sample_scenario(f"s{i}", seed=i), "") for i in (1, 2, 3)]
        b1 = RN.run_batch(self.ctx, items, self.dll["good"], jobs=2, repeat=2, name="t1")
        self.assertEqual(RN.batch_exit_code(b1), 0, b1["scenarios"])
        self.assertFalse(any(r["repeat_mismatch"] or r["cached"] for r in b1["scenarios"]))
        b2 = RN.run_batch(self.ctx, items, self.dll["good"], jobs=2, name="t2")
        self.assertTrue(all(r["cached"] for r in b2["scenarios"]))                 # 100% cache hits on a re-run
        entry = Path(b2["scenarios"][0]["run_dir"])
        self.assertTrue((entry / ".complete").is_file())
        self.assertEqual(digestbin.parse(entry / "digest.bin").last_turn, 8)
        self.assertFalse(list(entry.glob("*.db")))                                 # the host's data profile copy is pruned
        self.assertTrue((entry / "host.log.gz").is_file())
        self.assertTrue((Path(b2["dir"]) / "report.md").is_file())

    def test_crash_is_classified_and_cached_negative(self) -> None:
        item = S.Item("c", F.sample_scenario("c"), "")
        r = RN.run_scenario(self.ctx, item, self.dll["crash"])
        self.assertEqual((r["status"], r["last_turn"], r["turns_done"]), ("crash-repro", 3, 3))
        self.assertEqual(len(r["attempts"]), 2)                                    # one retry, same turn again
        again = RN.run_scenario(self.ctx, item, self.dll["crash"])
        self.assertTrue(again["cached"] and again["status"] == "crash-repro")
        self.assertEqual(RN.batch_exit_code({"scenarios": [again]}), 1)


if __name__ == "__main__":
    unittest.main()
