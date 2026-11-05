"""Tests for civsim.saveio on SYNTHETIC saves only (no real save data in the repo):
python -m unittest discover -s headless\\tools\\tests -p "test_saveio*.py"."""
from __future__ import annotations

import json
import os
import random
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from civsim.saveio import corpus, synth  # noqa: E402
from civsim.saveio import envelope as E  # noqa: E402
from civsim.saveio import extract as X  # noqa: E402
from civsim.saveio import report  # noqa: E402
from civsim.saveio import segments  # noqa: E402

NICK = "^Synth^@76561190000000001"
STEAMID = "76561190000000001"


class SaveioTests(unittest.TestCase):
    def test_envelope_parse_accounts_for_every_byte(self) -> None:
        data = synth.build_save(turn=7, humans=1, ais=3)
        env = E.parse_envelope(data)
        self.assertEqual((env.status, env.reason), ("supported", ""))
        self.assertEqual(env.coverage_problems(), [])                       # gaps/overlaps would be listed here
        self.assertEqual(sum(s.len for s in env.spans), len(data))
        sm = E.summarize(env)
        self.assertEqual((sm["turn"], sm["players"], sm["humans"], sm["lekmod_tag"]), (7, 4, 1, "LEKMOD v34.15"))
        self.assertEqual(sm["dlc_count"], 2)
        self.assertEqual(env.inflate.body, synth.synth_body())
        self.assertTrue(env.inflate.ends_with_sync_flush and not env.inflate.complete)   # sync flush, no final block
        names = {s.name for s in env.spans}
        self.assertTrue({"hdr.magic", "slot_hints.version", "archive.version_string", "exe.zstream.version"} <= names)

    def test_chunked_zlib_stream(self) -> None:
        rnd = random.Random(1)
        body = synth.synth_body(tail=0) + bytes(rnd.getrandbits(8) for _ in range(180_000))   # incompressible: > 2 chunks
        data = synth.build_save(body=body)
        env = E.parse_envelope(data)
        zs = env.zstream
        self.assertGreaterEqual(len(zs.chunk_lens), 3)
        self.assertTrue(all(n == E.CHUNK_SIZE for n in zs.chunk_lens[:-1]))
        self.assertTrue(0 < zs.chunk_lens[-1] <= E.CHUNK_SIZE and zs.clean and zs.version == 2)
        self.assertEqual(env.inflate.body, body)
        # the chunk headers are NOT part of the deflate stream: inflating the raw tail corrupts it after the first 64 KiB
        raw = data[env.zstream_off + 8:]
        with self.assertRaises(zlib.error):
            zlib.decompressobj().decompress(raw)

    def test_unsupported_and_garbage_do_not_crash(self) -> None:
        old = E.parse_envelope(synth.build_save(tag="Lekmod v33.11"))
        self.assertEqual(old.status, "unsupported")
        self.assertIn("33.11", old.reason)
        self.assertEqual(old.coverage_problems(), [])
        prefixed = E.parse_envelope(synth.build_save(tag="SOMEHOSTLEKMOD v34.15"))     # host-typed prefix before the tag
        self.assertEqual(prefixed.status, "supported")
        good = synth.build_save()
        cases = [b"", b"CIV5", b"CIV5" + b"\xff" * 64, good[:300], good[:len(good) // 2], good[:E.parse_envelope(good).zstream_off + 6]]
        flipped = bytearray(good)
        flipped[E.parse_envelope(good).zstream_off + 20] ^= 0xFF
        cases.append(bytes(flipped))
        for blob in cases:
            env = E.parse_envelope(blob)
            self.assertIn(env.status, ("unsupported", "no-body", "error", "supported"))
            self.assertEqual(env.coverage_problems(), [], (len(blob), env.status, env.reason))   # nothing dropped, even if unsupported
        self.assertEqual(E.parse_envelope(good[:E.parse_envelope(good).pregame_end]).status, "no-body")     # configuration save

    def test_masking_and_corpus_snapshot_are_read_only(self) -> None:
        self.assertEqual(report.mask_text(f"x {NICK} y {STEAMID} C:\\Users\\bob\\z"), "x player# y steamid# <user>\\z")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src, out = root / "Saves", root / "out" / "corpus"
            (src / "auto").mkdir(parents=True)
            a, b = synth.build_save(turn=3, nickname=NICK), synth.build_save(turn=4, nickname=NICK)
            (src / "A.Civ5Save").write_bytes(a)
            (src / "auto" / "B.Civ5Save").write_bytes(b)
            (src / "auto" / "A_copy.Civ5Save").write_bytes(a)                 # same bytes: one stored copy, two index rows
            (src / "ignore.txt").write_text("x")
            before = {p: (p.stat().st_mtime_ns, p.read_bytes()) for p in src.rglob("*") if p.is_file()}
            idx = corpus.snapshot(src, out=out, verbose=False)
            after = {p: (p.stat().st_mtime_ns, p.read_bytes()) for p in src.rglob("*") if p.is_file()}
            self.assertEqual(before, after)                                  # sources untouched
            self.assertEqual((idx["counts"]["files"], idx["counts"]["unique"], idx["counts"]["supported"]), (3, 2, 2))
            self.assertEqual(len(list((out / "saves").glob("*.Civ5Save"))), 2)
            texts = [(out / "index.json").read_text(encoding="utf-8")]
            ex = X.extract(a, root / "x")
            texts.append((root / "x" / "envelope.json").read_text(encoding="utf-8"))
            texts.append(json.dumps(ex))
            for t in texts:
                self.assertNotIn(STEAMID, t)
                self.assertNotIn("Synth^", t)
            self.assertIn(STEAMID.encode(), (root / "x" / "pregame.bin").read_bytes())    # raw bytes stay exact in the extract
            with self.assertRaises(SystemExit):                              # never write under Documents\My Games
                corpus.snapshot(src, out=root / "My Games" / "corpus", verbose=False)

    def test_extract_and_segments_reassemble(self) -> None:
        data = synth.build_save(turn=9)
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "ex"
            j = X.extract(data, out, with_zstream=True)
            hdr, pre, body = ((out / n).read_bytes() for n in ("header.bin", "pregame.bin", "body.bin"))
            self.assertEqual(len(hdr), j["layout"]["header"]["length"])
            self.assertEqual(hdr + pre, data[:j["layout"]["zstream"]["offset"]])
            self.assertEqual(body, synth.synth_body())
            self.assertEqual(zlib.decompressobj().decompress((out / "zstream.bin").read_bytes()), body)
            again = synth.assemble_save(hdr, pre, body)                      # export direction: same envelope, same body
            e2 = E.parse_envelope(again)
            self.assertEqual((e2.status, e2.inflate.body), ("supported", body))
            self.assertEqual(j["layout"]["zstream"]["deflate"]["reproduced"], True)       # level-6 recipe reproduces our own stream
            e3 = E.parse_envelope(synth.assemble_save(hdr, pre, body, partial_flush=True))   # the 13% variant is still a valid save
            self.assertEqual((e3.status, e3.inflate.body), ("supported", body))
            self.assertEqual(again[:j["layout"]["zstream"]["offset"]], data[:j["layout"]["zstream"]["offset"]])
            self.assertIn("exe.zstream.version", j["opaque_blobs"])
            self.assertEqual(json.loads((out / "envelope.json").read_text(encoding="utf-8"))["schema"], 1)
            seg = j["body_segments"]
            names = [s["name"] for s in seg["segments"]]
            self.assertEqual(names, ["game.fields", "game.sqlite_size", "game.sqlite", "map.header", "map.plots", "rest"])
            self.assertEqual((seg["plots"]["found"], seg["plots"]["expected"], seg["game_head"]["turn_slice"]), (12, 12, 777))
            self.assertEqual(sum(s["length"] for s in seg["segments"]), len(body))      # the segments tile the body


if __name__ == "__main__":
    unittest.main()
