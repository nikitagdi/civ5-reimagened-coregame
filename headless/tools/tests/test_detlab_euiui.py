"""Tests for civsim.detlab.euiui that need no game install: python -m unittest discover -s headless\\tools\\tests -p "test_detlab_euiui.py"."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from civsim.detlab import euiui as E  # noqa: E402


def facts(*hits: E.Hit) -> dict:
    return {"hits": list(hits), "includes": [], "lines": 1}


class BatTests(unittest.TestCase):
    BAT = (
        '@echo off\r\n'
        'IF EXIST "UI_bc1" (\r\n'
        '  copy /y "LEKMOD_X\\Lua\\tmp\\eui\\A.lua.ignore" "LEKMOD_X\\Lua\\UI\\A.lua" > nul\r\n'
        ') ELSE (\r\n'
        '  copy /y "LEKMOD_X\\Lua\\tmp\\ui\\A.lua.ignore" "LEKMOD_X\\Lua\\UI\\A.lua" > nul\r\n'
        ')\r\n'
        'copy /y "LEKMOD_X\\Lua\\tmp\\ui\\InGame.lua.ignore" "LEKMOD_X\\Lua\\UI\\InGame.lua" > nul\r\n'
    )

    def test_parse_and_run_both_branches(self) -> None:
        nodes = E.parse_bat(self.BAT)
        self.assertEqual([n.op for n in nodes], ["if", "copy"])
        with tempfile.TemporaryDirectory() as td:
            dlc = Path(td) / "DLC"
            lk = dlc / "LEKMOD_X"
            for sub in ("eui", "ui"):
                (lk / "Lua" / "tmp" / sub).mkdir(parents=True)
                (lk / "Lua" / "tmp" / sub / "A.lua.ignore").write_text("x", encoding="latin-1")
            (lk / "Lua" / "tmp" / "ui" / "InGame.lua.ignore").write_text("g", encoding="latin-1")
            for eui in (True, False):
                r = E.BatRun(lk, dlc, eui, dlc / "UI_bc1")
                r.env["patchfolder"] = str(lk)
                r.run(E.parse_bat(self.BAT.replace("LEKMOD_X", str(dlc / "LEKMOD_X"))))
                srcs = {d: s for d, s, ok, _ in r.copies if ok}
                self.assertIn("InGame.lua", srcs)          # always from tmp\ui
                self.assertTrue(srcs["A.lua"].startswith("eui/" if eui else "ui/"))


class LuaBlockTests(unittest.TestCase):
    def test_extract_call_block_balances_parentheses_and_ignores_strings(self) -> None:
        raw = ('local x = 1\n'
               'GameEvents.CityBoughtPlot.Add(\n'
               '\tfunction( p, c )  -- ) in a comment\n'
               '\t\tprint( "a ) b" )\n'
               '\tend)\n'
               'local y = 2\n')
        blk = E.extract_call_block(raw, r"\bGameEvents\.CityBoughtPlot\.Add\s*\(")
        self.assertIsNotNone(blk)
        self.assertTrue(blk.startswith("GameEvents.CityBoughtPlot.Add("))
        self.assertTrue(blk.rstrip().endswith("end)"))
        self.assertNotIn("local y", blk)
        self.assertIsNone(E.extract_call_block(raw, r"GameEvents\.Nothing\.Add\s*\("))


class ClassifyTests(unittest.TestCase):
    def test_automatic_mutator_difference_is_sim_auto(self) -> None:
        add = E.Hit("mutate", "p:AddNotification", 10, "<anon@GameEvents.CityBoughtPlot.Add>", "GameEvents:CityBoughtPlot", "Player")
        cls, _ = E.classify("somepanel", facts(add), facts())
        self.assertEqual(cls, E.SIM_AUTO)

    def test_click_only_command_difference_is_sim_click(self) -> None:
        net = E.Hit("net", "Network.SendResearch", 5, "OnClick", "callback")
        cls, note = E.classify("sometree", facts(net), facts())
        self.assertEqual(cls, E.SIM_CLICK)
        self.assertIn("Network.SendResearch", note)

    def test_stub_and_identical_commands_are_ui_only(self) -> None:
        net = E.Hit("net", "Network.SendResearch", 5, "OnClick", "callback")
        self.assertEqual(E.classify("same", facts(net), facts(net))[0], E.UI_ONLY)
        self.assertEqual(E.classify("somepopup", facts(), facts(net), size_eui=41, size_ui=20000)[0], E.UI_ONLY)


class BotTests(unittest.TestCase):
    def test_bot_and_probe_are_plain_lua_text(self) -> None:
        for src in (E.BOT_LUA, E.MIGHT_PROBE_LUA):
            self.assertEqual(src.count("("), src.count(")"))
            self.assertNotIn("\r", src)
        self.assertIn("DoJONSCultureLevelIncrease", E.BOT_LUA)
        self.assertIn("GetMilitaryMight", E.MIGHT_PROBE_LUA)


if __name__ == "__main__":
    unittest.main()
