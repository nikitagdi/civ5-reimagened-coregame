"""civsim.detlab.oos: turn-log parser and comparison (synthetic logs, no game files)."""
from __future__ import annotations

import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from civsim.detlab import oos


def turn_lines(turn: int, ap: int, jon_calls: int, p01_units: str = "bbbbbbbb", stamp: bool = False) -> list[str]:
    pre = f"[{100 + turn}.250] " if stamp else ""
    return [
        f"{pre}LTL1 t={turn} ap={ap} jon=123{turn},{jon_calls},3 map=99,500,2 g=0000000{turn % 10} m=aaaaaaaa v=cccccccc us={ap * 7} kc={ap + 1000}",
        f"{pre}LTL1 t={turn} T00=3,11111111 T01=2,22222222",
        f"{pre}LTL1 t={turn} P00=h,1,3,500,e0e0e0e0,c0c0c0c0,u0u0u0u0,p0p0p0p0 P01=a,2,5,800,e1e1e1e1,c1c1c1c1,{p01_units},p1p1p1p1",
    ]


def make_log(ap: int, diverge_at: int | None = None, turns: int = 8, stamp: bool = False) -> str:
    lines = ["LekmodNG turn log v1 (LTL1)"]
    for t in range(turns):
        bad = diverge_at is not None and t >= diverge_at
        lines += turn_lines(t, ap, 1000 + 10 * t + (4 if bad else 0), "dddddddd" if bad else "bbbbbbbb", stamp)
    return "\n".join(lines) + "\n"


class TurnLogTests(unittest.TestCase):
    def test_parse_with_timestamps_and_duplicates(self):
        text = make_log(0, stamp=True) + "\n".join(turn_lines(3, 0, 1, "ffffffff")) + "\n"   # a reload logs turn 3 again
        log = oos.parse_turnlog(text)
        self.assertEqual(sorted(log.records), list(range(8)))
        self.assertEqual(log.duplicates, {3: 1})
        self.assertEqual(log.records[3]["P01"].split(",")[6], "bbbbbbbb")                      # the first record wins
        self.assertEqual(log.active_player, "0")
        self.assertEqual(log.records[5]["T01"], "2,22222222")

    def test_equal_logs_ignore_active_player_and_cost(self):
        res = oos.compare_turnlogs({"a": oos.parse_turnlog(make_log(0)), "b": oos.parse_turnlog(make_log(1))})
        self.assertIsNone(res["first_divergence"])
        self.assertEqual(res["last_equal_turn"], 7)

    def test_first_divergence(self):
        res = oos.compare_turnlogs({"a": oos.parse_turnlog(make_log(0)), "b": oos.parse_turnlog(make_log(1, diverge_at=5)),
                                    "c": oos.parse_turnlog(make_log(2))})
        fd = res["first_divergence"]
        self.assertEqual(fd["turn"], 5)
        self.assertEqual(res["last_equal_turn"], 4)
        keys = {d["key"]: d for d in fd["diffs"]}
        self.assertEqual(set(keys), {"jon", "P01"})
        self.assertEqual(keys["P01"]["groups"], [["a", "c"], ["b"]])
        self.assertIn("units_hash", keys["P01"]["detail"])
        self.assertIn("(+4)", keys["jon"]["detail"])

    def test_zip_and_dir_inputs_end_to_end(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            z1 = td / "LekmodReport-PC1-20261017-2000.zip"
            with zipfile.ZipFile(z1, "w") as z:
                z.writestr("Logs/LekmodNG-turns.log", make_log(0))
                z.writestr("Logs/net_message_debug.log", "x\nGame Random Number Generators are out of sync : local.seed=1, remote.seed=2\n")
                z.writestr("Logs/RandCalls.csv", "Game Turn, Turn Slice, Range, Value, Seed, Instance, Type, Location\n5,1,10,3,77,0x1234,sync,A\n5,1,10,4,78,0x1234,sync,B\n")
                z.writestr("Saves/AutoSave_Post_0004.Civ5Save", b"")
                z.writestr("report.json", json.dumps({"computer_name": "PC1", "release": "r3"}))
            d2 = td / "pc2"
            (d2 / "Logs").mkdir(parents=True)
            (d2 / "Logs" / "LekmodNG-turns.log").write_text(make_log(1, diverge_at=5))
            (d2 / "Logs" / "RandCalls.csv").write_text("Game Turn, Turn Slice, Range, Value, Seed, Instance, Type, Location\n5,1,10,3,77,0x9999,sync,A\n5,1,12,4,78,0x9999,sync,C\n")
            res, text = oos.analyse([str(z1), str(d2)])
            self.assertEqual(res["turnlogs"]["first_divergence"]["turn"], 5)
            self.assertEqual(res["randcalls"]["first_difference"]["pc2"]["index"], 1)          # Instance column ignored
            self.assertIn("FIRST DIVERGENT TURN 5", text)
            self.assertIn("rng 1", text)
            self.assertIn("AutoSave_Post_0004", text)
            self.assertEqual(oos.main([str(z1), str(d2)]), 6)
            self.assertEqual(oos.main([str(z1)]), 2)


if __name__ == "__main__":
    unittest.main()
