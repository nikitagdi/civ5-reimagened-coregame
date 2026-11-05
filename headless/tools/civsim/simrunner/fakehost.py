"""Fake civhost for tests and `simrunner selftest`: same CLI and output files as civhost (digest.bin v1, run.json, state-T/*.bin),
state is a pure function of (scenario, turn). Behaviour is steered by text tags inside the "DLL" file:

  MS=<float>           base ms per turn (default 10)
  DIVERGE=<turn>:<seg> from <turn> on, segment <seg> hashes differently (state dump differs at byte 100)
  CRASH_AT=<turn>      exit 3 right after the digest record of that turn (no run.json)
  NONSER               state after --load differs for segment player00 (a non-serialized field, class C1)
  C2                   segment player00 depends on --heap-pattern (uninitialized memory, class C2)
  C5                   segment player00 depends on --active-player (class C5)
"""
from __future__ import annotations

import hashlib
import json
import os
import random
import re
import sys
import time
from pathlib import Path

USAGE = """fakehost 0.1
usage: fakehost.py --dll <dll> --data <dir> [--game <dir>] --scenario <json> --turns N --out <dir>
                   [--dump-state-at T] [--checkpoint-every N] [--load <ckpt>] [--active-player <slot>] [--heap-pattern <p>] [--strict]
exit: 0 ok, 1 setup, 3 crash, 7 stuck
"""


def _h(*parts) -> int:
    return int.from_bytes(hashlib.blake2b("|".join(map(str, parts)).encode(), digest_size=8).digest(), "little")


def main(argv: list[str]) -> int:
    opts: dict[str, str] = {}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--strict",):
            opts[a] = "1"
        elif a.startswith("--") and a != "--help" and i + 1 < len(argv):
            i += 1
            opts[a] = argv[i]
        else:
            print(USAGE)
            return 1
        i += 1
    for need in ("--dll", "--scenario", "--turns", "--out"):
        if need not in opts:
            print(USAGE)
            return 1
    dll = Path(opts["--dll"]).read_text(encoding="utf-8", errors="replace")
    scn = json.loads(Path(opts["--scenario"]).read_text(encoding="utf-8-sig"))
    out = Path(opts["--out"])
    out.mkdir(parents=True, exist_ok=True)
    (out / "gameplay.db").write_bytes(b"not a real db" * 100)      # host copies the data profile into the run dir

    def tag(name: str, default: str | None = None) -> str | None:
        m = re.search(rf"{name}=([^\s]+)", dll)
        return m.group(1) if m else default

    base_ms = float(tag("MS", "10"))
    div = tag("DIVERGE")
    div_turn, div_seg = (int(div.split(":")[0]), div.split(":")[1]) if div else (None, None)
    crash_at = int(tag("CRASH_AT", "0"))
    seed = _h(json.dumps({k: v for k, v in scn.items() if k not in ("name", "note", "turns", "env", "active_player")}, sort_keys=True))
    npl = len(scn.get("civs", [])) or 2
    segs = ["game", "map", "team00", "team01"] + [f"player{p:02d}" for p in range(npl)] + ["supporting"]
    start = 0
    loaded = False
    if "--load" in opts:
        start = json.loads(Path(opts["--load"]).read_text())["turn"]
        loaded = True
    n_turns = int(opts["--turns"])
    dump_at = int(opts["--dump-state-at"]) if "--dump-state-at" in opts else None
    ck_every = int(opts.get("--checkpoint-every", 0))
    rng = random.Random(_h(str(out.name), dll))

    def extras(seg: str, turn: int) -> list:
        extra = []
        if div_turn is not None and seg == div_seg and turn >= div_turn:
            extra.append("diverged")
        if seg == "player00":
            if "C2" in dll:
                extra.append(opts.get("--heap-pattern", ""))
            if "C5" in dll:
                extra.append(opts.get("--active-player", scn.get("active_player", 0)))
            if "NONSER" in dll and loaded and turn > start:
                extra.append("nonser")
        return extra

    def seg_hash(seg: str, turn: int) -> int:
        return _h(seed, seg, turn, *extras(seg, turn))

    def seg_bytes(seg: str, turn: int) -> bytes:
        b = bytearray(hashlib.blake2b(f"{seed}|{seg}|{turn}".encode(), digest_size=64).digest() * 4)    # 256 bytes
        ex = extras(seg, turn)
        if ex:
            b[100] ^= (_h(*ex) & 0xFF) | 1      # a perturbed segment differs at byte 100, in a way that depends on the perturbation
        return bytes(b)

    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from civsim import digestbin     # noqa: E402

    records: list[tuple[int, list[int]]] = []
    turn_rows = []
    dpath = out / "digest.bin"
    log = open(out / "civhost.log", "w")
    log.write(f"fakehost start turn {start}\n")

    def flush() -> None:
        dpath.write_bytes(digestbin.build(segs, records))

    if dump_at is not None and dump_at == start:
        d = out / f"state-{start}"
        d.mkdir(exist_ok=True)
        for s in segs:
            (d / f"{s}.bin").write_bytes(seg_bytes(s, start))
    for k in range(1, n_turns + 1):
        t = start + k
        hs = [seg_hash(s, t) for s in segs]
        records.append((t, hs))
        flush()
        ms = base_ms * (1 + t / 50.0) * (1 + rng.uniform(-0.03, 0.03))
        turn_rows.append({"turn": t, "slices": 3, "ms": round(ms, 2), "digest_ms": 0.1, "state_bytes": 256 * len(segs), "net": 0,
                          "digest": f"{hs[0]:016x}"})
        if dump_at == t:
            d = out / f"state-{t}"
            d.mkdir(exist_ok=True)
            for s in segs:
                (d / f"{s}.bin").write_bytes(seg_bytes(s, t))
        if ck_every and t % ck_every == 0:
            (out / "checkpoints").mkdir(exist_ok=True)
            (out / "checkpoints" / f"turn-{t:04d}.ckpt").write_text(json.dumps({"turn": t}))
        log.write(f"turn {t} {ms:.1f} ms\n")
        if crash_at and t == crash_at:
            log.write("CRASH\n")
            log.close()
            return 3
    log.close()
    total = sum(r["ms"] for r in turn_rows)
    (out / "run.json").write_text(json.dumps({
        "host": "fakehost 0.1", "status": "ok", "scenario": opts["--scenario"], "dll": opts["--dll"], "turns_requested": n_turns,
        "turns_completed": n_turns, "start_turn": start, "end_turn": start + n_turns, "digest": "digest.bin", "digest_segments": len(segs),
        "final_digest": f"{records[-1][1][0]:016x}", "launch_ms": 5.0, "total_turn_ms": round(total, 1), "mean_turn_ms": round(total / n_turns, 2),
        "turns": turn_rows}, indent=1))
    return 0


def make_sandbox(root: Path) -> dict:
    """Throw-away environment for tests, laid out like headless/out: oracle/cache-fake, dll/<alias>/, runs, batches."""
    root.mkdir(parents=True, exist_ok=True)
    data = root / "oracle" / "cache-fake"
    data.mkdir(parents=True, exist_ok=True)
    (data / "Civ5DebugDatabase.db").write_bytes(b"fake data profile")
    dlls = {}
    for name, text in {"good": "MS=10", "fast": "MS=5", "bad": "MS=10 DIVERGE=4:player01", "crash": "MS=10 CRASH_AT=3",
                       "c5": "MS=10 C5", "nonser": "MS=10 NONSER"}.items():
        p = root / "dll" / name / "CvGameCore_Expansion2.dll"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        dlls[name] = str(p)
    host = str(Path(__file__).resolve())
    return {"host": host, "data": str(data), "runs_root": root / "runs", "batches_root": root / "batches", "dlls": dlls,
            "env": {"CIVSIM_OUT": str(root), "CIVHOST": host}}      # env makes the CLIs run against the sandbox (aliases good/fast/bad/crash)


def sample_scenario(name: str = "fake", seed: int = 1, turns: int = 8, civs: int = 3) -> dict:
    return {"name": name, "seeds": {"sync": seed, "map": seed + 1}, "world_size": "WORLDSIZE_STANDARD", "turns": turns,
            "civs": [{"civ": f"CIVILIZATION_FAKE{i}", "leader": f"LEADER_FAKE{i}"} for i in range(civs)], "minors": []}


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
