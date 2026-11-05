"""Minimal tests for civsim.detlab: python -m unittest discover -s headless\\tools\\tests -p "test_detlab*.py"."""
from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from civsim.detlab import catalog as CAT  # noqa: E402
from civsim.detlab import scan as S  # noqa: E402


class PreprocessTests(unittest.TestCase):
    def test_ifdef_else_and_block_comments(self) -> None:
        src = "#define A\n/*\n#define B\n*/\n#ifdef A\nint a;\n#else\nint b;\n#endif\n#ifdef B\nint c;\n#endif\n"
        out = S.preprocess(S.strip_cpp_comments(src), {}).split("\n")
        self.assertIn("int a;", out[5])
        self.assertEqual(out[7].strip(), "")        # #else branch dropped
        self.assertEqual(out[10].strip(), "")       # B is inside a block comment, so undefined
        self.assertEqual(len(out), len(src.split("\n")))   # line numbers preserved


class LuaRuleTests(unittest.TestCase):
    LUA = (
        "function on_adopt(pid, pol)\n"
        "  local player = Players[pid]\n"
        "  if Game.GetActivePlayer() ~= pid then return end\n"
        "  player:ChangeGoldenAgeTurns(5)\n"
        "  GameEvents.PlayerAdoptPolicy.Remove(on_adopt)\n"
        "end\n"
        "GameEvents.PlayerAdoptPolicy.Add(on_adopt)\n"
        "function dead_helper() return Game.Rand(3, '') end\n"
    )

    def setUp(self) -> None:
        self.f = S._scan_lua_file(Path("synthetic.lua"), self.LUA)

    def test_activeplayer_guard_reported_at_test_line(self) -> None:
        hits = [f for f in self.f if f.rule == "lua.activeplayer"]
        self.assertEqual([(h.line, h.score) for h in hits], [(3, 3)])

    def test_selfremove(self) -> None:
        self.assertEqual([f.line for f in self.f if f.rule == "lua.selfremove"], [5])

    def test_unreachable_rand_downgraded(self) -> None:
        # not a UI file and never called: no lua.rand-ui at score >= 2
        self.assertFalse([f for f in self.f if f.rule == "lua.rand-ui" and f.score >= 2])


class RepoScanTests(unittest.TestCase):
    """Success criteria for the static scanners, at HEAD so parallel edits do not move lines."""

    @classmethod
    def setUpClass(cls) -> None:
        S._REV, S._GLOBAL_DEFINES = "bf3f4e9", None  # original v34.15: the known bugs are fixed on HEAD
        idx = S.CppIndex()
        cls.cpp = S.scan_uninit(idx) + S.scan_nonser(idx)
        cls.lua = S.scan_lua([S.REPO / "LEKMOD" / "Lua"])

    @classmethod
    def tearDownClass(cls) -> None:
        S._REV, S._GLOBAL_DEFINES = None, None

    def test_uninit_finds_reformation_flag(self) -> None:
        self.assertTrue(any(f.rule == "uninit.member" and "m_bCanChooseReformationBelief" in f.msg for f in self.cpp))

    def test_nonser_finds_ideology_bonus(self) -> None:
        self.assertTrue(any(f.rule == "nonser.member" and "m_iCombatBonusVsDifferentIdeologyModifier" in f.msg
                            for f in self.cpp))

    def test_lua_finds_italy_line_21(self) -> None:
        self.assertTrue(any(f.rule == "lua.activeplayer" and f.file.endswith("Lekmod_italy.lua") and f.line == 21
                            for f in self.lua))


class CatalogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cat = CAT.load()

    def test_schema_valid(self) -> None:
        self.assertEqual(CAT.check_schema(self.cat), [])

    def test_seed_ids_kept(self) -> None:
        ids = {e["id"] for e in self.cat["entries"]}
        self.assertTrue({"DS-0001", "DS-0002", "DS-0003", "DS-0004", "DS-0005"} <= ids)

    def test_bad_entry_reported(self) -> None:
        cat = copy.deepcopy(self.cat)
        cat["entries"][0]["status"] = "maybe"
        cat["entries"][0]["confidence"] = 2
        errs = CAT.check_schema(cat)
        self.assertTrue(any("bad status" in e for e in errs) and any("confidence" in e for e in errs), errs)

    def test_render_ranks_and_lists_top10(self) -> None:
        md = CAT.render(self.cat)
        top = md.split("## Top 10 to confirm first", 1)[1].split("## Summary", 1)[0]
        numbered = [ln for ln in top.split("\n") if ln[:1].isdigit()]
        self.assertEqual(len(numbered), min(10, len(self.cat["entries"])))
        best = CAT.ranked(self.cat)[0]["id"]
        self.assertIn(f"1. **{best}**", top)


if __name__ == "__main__":
    unittest.main()
