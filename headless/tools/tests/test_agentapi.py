"""Minimal tests for civsim.agentapi: python -m unittest discover -s headless\\tools\\tests -p "test_agentapi*.py"."""
from __future__ import annotations

import contextlib
import copy
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from civsim.agentapi import __main__ as cli  # noqa: E402
from civsim.agentapi import canon, content, coverage, dllparse, render, schema  # noqa: E402

HAVE_DLL = (canon.DLL_SRC / dllparse.UTILITY_H).exists()
FIXES = json.loads((canon.REPO / "headless" / "improvements" / "fixes.json").read_text(encoding="utf-8"))["fixes"]


class DocumentTests(unittest.TestCase):
    def test_documents_valid_canonical_and_hash_stable(self) -> None:
        code, errs, h = cli.run_check()
        self.assertEqual((code, errs), (0, []))
        for n in canon.FILES:
            self.assertTrue(canon.is_canonical(n), n)
        doc = canon.load("commands")
        self.assertEqual(h, canon.dict_hash(doc))
        self.assertEqual(h, __import__("hashlib").sha256(canon.path_of("commands").read_bytes()).hexdigest())
        # the validator and the semantic rules catch broken documents
        self.assertTrue(schema.validate({"schema": 1}, schema.S_ERRORS))
        bad = copy.deepcopy(doc)
        c = next(x for x in bad["commands"] if x["name"] == "city.push_order")
        c["validate"] = [r for r in c["validate"] if not r.startswith("ownership")]
        c["dll_gaps"] = []
        errs = schema.check_commands(bad, FIXES)
        self.assertTrue(any("ownership" in e for e in errs) and any("gap ids" in e for e in errs), errs)

    @unittest.skipUnless(render.OUT.exists(), "generated headless/generated-docs/AGENT-COMMANDS.md is not committed (run: python -m civsim.agentapi render)")
    def test_committed_commands_document_is_current(self) -> None:
        doc = canon.load("commands")
        self.assertEqual(render.render(doc, canon.dict_hash(doc)), render.render(doc, canon.dict_hash(doc)))
        self.assertFalse(render.write(doc, canon.dict_hash(doc), check_only=True), "run: python -m civsim.agentapi render")


@unittest.skipUnless(HAVE_DLL, "DLL sources not present")
class CoverageTests(unittest.TestCase):
    def test_every_wire_name_is_classified_and_unknown_fails(self) -> None:
        doc = canon.load("commands")
        w = coverage.collect(canon.DLL_SRC, [])
        rep = coverage.classify(doc, w)
        self.assertEqual((rep.unknown, rep.stale), ([], []))
        self.assertEqual(rep.code, 0)
        self.assertEqual(w.response_decls, 64)
        # a new send*/Response* and a new Lua Network.Send* are unknown -> exit code 2
        w.send["sendFoo"] = "ICvDLLUtility.h:1"
        self.assertEqual((coverage.classify(doc, w).unknown, coverage.classify(doc, w).code), ([("send", "sendFoo")], 2))
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "a.lua").write_text("Network.SendFoo(1)\n-- Network.SendBar()\n--[[ Network.SendBaz() ]]\n", encoding="utf-8")
            self.assertEqual(list(dllparse.scan_lua([Path(d)])), ["SendFoo"])
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cli.main(["coverage", "--lua-root", d]), 2)
        # the observation 'pending' table has one row per ENDTURN_BLOCKING_* type
        obs = canon.load("observation")
        self.assertEqual([p["type"] for p in obs["pending"]], content.blocking_types(canon.DLL_SRC))

    def test_fix_validation_records_line_up_with_gaps_and_code(self) -> None:
        doc = canon.load("commands")
        self.assertEqual(schema.check_fix_alignment(doc, FIXES), [])
        self.assertEqual(coverage.check_guards(doc, canon.DLL_SRC), [])
        ids = {g["id"] for c in doc["commands"] for g in c["dll_gaps"] if g["id"].startswith("FIX-")}
        self.assertEqual(ids, {r["id"] for r in FIXES if r["kind"] == "validation"})
        stale = copy.deepcopy(FIXES)
        next(r for r in stale if r["id"] == "FIX-0103")["status"] = "implemented"
        self.assertTrue(schema.check_fix_alignment(doc, stale))


class ContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.doc = canon.load("commands")
        self.by = schema.by_name(self.doc)

    def codes(self, name: str, args: dict, player: int = 2) -> list[str]:
        return [c for c, _ in schema.check_call(self.doc, name, args, player)]

    def test_multiplexers_classes_and_bot_commands(self) -> None:
        self.assertEqual(self.codes("minor.gift_unit", {"minor": 30, "unit": -2}), ["reserved_value"])
        self.assertEqual(self.codes("minor.gift_unit", {"minor": 30, "unit": {"owner": 5, "id": 7}}), ["not_owner"])
        self.assertEqual(self.codes("minor.gift_gold", {"minor": 30, "gold": 100001}), ["reserved_value"])
        self.assertEqual(self.codes("minor.gift_gold", {"minor": 30, "gold": -5}), ["out_of_range"])
        self.assertEqual(self.codes("minor.gift_gold", {"minor": 3, "gold": 50}), ["out_of_range"])  # a major is not a city-state
        self.assertEqual(self.codes("minor.gift_gold", {"minor": 30, "gold": 50}), [])
        self.assertEqual(self.codes("server.sync_unit", {}), ["forbidden"])
        self.assertEqual(self.codes("policy.remove", {}), ["forbidden"])
        self.assertEqual(self.codes("nope.nope", {}), ["unknown_command"])
        for n in ("mp.turn_timer_reset", "mp.propose_irr", "mp.propose_cc", "mp.propose_scrap", "mp.vote", "mp.turn_timer_pause",
                  "stats.demographics_opened", "replay.chat", "minor.gift_faith"):
            self.assertIn(self.by[n]["class"], ("lekmod_mp", "meta"), n)
        self.assertEqual({self.by[n]["class"] for n in ("server.autopause_on", "server.autopause_off")}, {"server"})
        # the bot-player table: research, production, policy, automate, push mission, turn complete
        for n in ("player.research", "city.push_order", "policy.adopt", "unit.automate", "unit.push_mission", "turn.complete"):
            self.assertIn("bot", self.by[n]["tags"], n)
            self.assertTrue(any(r.split(":", 1)[0].endswith("/hand") for r in self.by[n]["validate"]), n)
        # scenario line {turn, player, cmd, args} (placeholder policy Type)
        line = json.loads('{"turn": 120, "player": 2, "cmd": "policy.adopt", "args": {"policy": "POLICY_TRADITION_FINISHER"}}')
        self.assertEqual(self.codes(line["cmd"], line["args"], line["player"]), [])
        self.assertEqual(self.codes("policy.adopt", {}), ["bad_args"])


if __name__ == "__main__":
    unittest.main()
