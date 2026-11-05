"""python -m civsim.lockstep run <session.json> [--peers N] [--dll D] [--until-turn T] ...
python -m civsim.lockstep selftest                                                          (unit checks, no civhost)

Exit codes: 0 all peers equal, 4 cross-peer divergence (desync candidate), 5 hang, 1 run error, 2 config error.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from .. import common as C
from .. import digestbin as D
from .coord import DEFAULTS, Session, bot_init_chunk, load_session, order_key, run_replay, run_single, turn_slices_on
from .localize import diff_clusters, localize, render_md
from .peer import PeerError


def _dll(spec: str | None) -> Path | None:
    if not spec:
        return None
    return C.resolve_dll(spec).path


def cmd_run(a) -> int:
    try:
        cfg = load_session(Path(a.session) if a.session else None, {
            "scenario": a.scenario, "until_turn": a.until_turn, "ai_sends": a.ai_sends, "order": a.order, "remote_delay": a.remote_delay,
            "turn_barrier": True if a.turn_barrier else None, "n_peers": a.peers, "name": a.name, "lua_overlay": a.lua_overlay,
            "stop_on_divergence": False if a.keep_going else None})
    except (OSError, ValueError) as e:
        print(f"config: {e}", file=sys.stderr)
        return 2
    if a.no_single:
        cfg["single"] = False
    if a.mask:
        cfg["mask"] = list(cfg.get("mask") or []) + a.mask
    if a.no_mask:
        cfg["mask"] = []
    exe = C.resolve_host(a.host).path
    dll = _dll(a.dll or cfg.get("dll") or "golden")
    diag = _dll(a.diag_dll or cfg.get("diag_dll"))
    cfg["mask"] = expand_mask(cfg.get("mask"), dll)      # '@tree' / '@bot' -> this DLL's source lines
    until = int(cfg["until_turn"])
    out = Path(a.out) if a.out else C.OUT / "sessions" / cfg["name"]
    out.mkdir(parents=True, exist_ok=True)
    print(f"lockstep {cfg['name']}: {len(cfg['peers'])} peers (players {[p['player'] for p in cfg['peers']]}), dll {dll.parent.name}, until turn {until}, out {out}")
    single = run_single(cfg, out / "single", exe, dll, until) if cfg.get("single", True) else None
    s = Session(cfg, out, exe, dll)
    t0 = time.time()
    try:
        s.start()
        res = s.run(until)
    except PeerError as e:
        res = dict(s.result, status="error", error=str(e))
        print(f"ERROR: {e}", file=sys.stderr)
    finally:
        s.close()
    res.update(name=cfg["name"], session=cfg, dll=str(dll), diag_dll=str(diag) if diag else None, host=str(exe), wall_s=round(time.time() - t0, 1),
               peers=[{"id": i, "player": p["player"], "run_dir": str(out / f"peer-{i}")} for i, p in enumerate(cfg["peers"])])
    # single-process reference vs peer 0
    if single is not None:
        rc = single.wait()
        sd = out / "single" / "digest.bin"
        if rc == 0 and sd.is_file() and (out / "peer-0" / "digest.bin").is_file():
            c = D.compare(D.parse(sd), D.parse(out / "peer-0" / "digest.bin"))
            res["single_vs_peer0"] = {k: c[k] for k in ("equal", "identical", "common_turns", "first_divergent_turn", "divergent_segments", "n_divergent_turns")}
            print(f"single-process network run vs peer 0: " + ("equal over %d turns" % c["common_turns"] if c["equal"] else
                  f"DIFFER from turn {c['first_divergent_turn']} ({', '.join(c['divergent_segments'][:6])})"))
        else:
            res["single_vs_peer0"] = {"error": f"single run exit {rc}"}
    loc = None
    if res["status"] == "divergent" and not a.no_localize:
        try:
            loc = localize(cfg, res, out, exe, dll, diag, log=print)
            res["localize"] = loc
        except (PeerError, OSError, ValueError, KeyError) as e:
            res["localize_error"] = str(e)
            print(f"localize failed: {e}", file=sys.stderr)
    C.write_json(out / "result.json", res)
    if res["status"] == "divergent":
        C.write_json(out / "divergence.json", {"divergences": res["divergences"], "localize": loc})
        (out / "divergence.md").write_text(render_md(cfg, res, loc), encoding="utf-8")
    print(f"result: {res['status']} after {res.get('frames')} frames, turns {res.get('turns')}, {res.get('wall_s')} s; messages {res.get('messages')}; {out / 'result.json'}")
    return {"equal": 0, "divergent": 4, "hang": 5}.get(res["status"], 1)


# Mask lines : unit cycler, CvPlot world anchor, tutorial latch. The line patterns are the PDB
# lines of the build, so they are derived from the DLL's source (tree_mask): the serializing statements below are looked up
# in the sources the DLL was built from (its manifest's git_sha when the build was clean, else the working tree).
# Fallback (HEAD 7966e9ff) if a source cannot be read:
TREE_MASK = ["cvunitcycler.cpp", "cvplot.cpp:11147|", "cvplot.cpp:11148|", "cvgame.cpp:11326|"]
MASK_STATEMENTS = [("CvPlot.cpp", "kStream << m_eWorldAnchor;"), ("CvPlot.cpp", "kStream << m_cWorldAnchorData;"),
                   ("CvGame.cpp", "kStream << m_bTutorialEverAttacked;")]
DLL_SRC = "LEKMOD_DLL/CvGameCoreDLL_Expansion2"
# Bot mode adds the two catalog mask candidates that only exist with a human slot: notifications added under active-player
# branches (CvNotifications, local UI list; the human's list differs between its own machine and the others by design) and
# the diplomacy request queue popped only on the target's machine (CvDiplomacyRequests).
BOT_EXTRA_MASK = ["cvnotifications.cpp", "cvdiplomacyrequests.cpp"]
BOT_MASK = TREE_MASK + BOT_EXTRA_MASK


def _dll_source_rev(dll: Path | None) -> str | None:
    """git_sha of a clean build (build-dll.ps1 manifest.json next to the DLL), else None (= working tree)."""
    if dll is None:
        return None
    try:
        m = C.read_json(Path(dll).parent / "manifest.json")
    except (OSError, ValueError):
        return None
    return m.get("git_sha") if m.get("git_sha") and m.get("git_dirty") is False else None


def _source_text(rel: str, rev: str | None) -> str:
    if rev:
        import subprocess
        r = subprocess.run(["git", "-C", str(C.REPO), "show", f"{rev}:{rel}"], capture_output=True)
        if r.returncode == 0:
            return r.stdout.decode("utf-8", "replace")
    return (C.REPO / rel).read_text(encoding="utf-8", errors="replace")


def tree_mask(dll: Path | None = None, log=None) -> list[str]:
    """['cvunitcycler.cpp', 'cvplot.cpp:L1|', 'cvplot.cpp:L2|', 'cvgame.cpp:L3|'] for the sources of `dll` (or the working tree).
    Each statement must occur exactly once in its file; otherwise the fallback TREE_MASK is returned (with a warning)."""
    rev = _dll_source_rev(dll)
    out = ["cvunitcycler.cpp"]
    try:
        cache: dict[str, list[str]] = {}
        for fname, stmt in MASK_STATEMENTS:
            if fname not in cache:
                cache[fname] = _source_text(f"{DLL_SRC}/{fname}", rev).splitlines()
            hits = [i + 1 for i, line in enumerate(cache[fname]) if line.strip() == stmt]
            if len(hits) != 1:
                raise ValueError(f"{fname}: '{stmt}' found {len(hits)} times")
            out.append(f"{fname.lower()}:{hits[0]}|")
    except (OSError, ValueError) as e:
        if log:
            log(f"tree_mask: {e}; using the fallback {TREE_MASK}")
        return list(TREE_MASK)
    return out


def expand_mask(masks: list[str] | None, dll: Path | None) -> list[str]:
    """'@tree' in a mask list = this DLL's tree masks (tree_mask); '@bot' = tree masks + the bot-mode extras."""
    res: list[str] = []
    for m in masks or []:
        if m == "@tree":
            res += tree_mask(dll, log=print)
        elif m == "@bot":
            res += tree_mask(dll, log=print) + BOT_EXTRA_MASK
        else:
            res.append(m)
    return list(dict.fromkeys(res))
# Repo Lua that differs from the installed Lekmod v34.15 (FIX-0003 Italy, FIX-0004 policies, FIX-0007 / finding 14 dummies)
REPO_FIXED_LUA = ["LEKMOD/Lua/Civilizations/Lekmod_italy.lua", "LEKMOD/Lua/Lekmod_policies.lua", "LEKMOD/Lua/Lekmod_global_dummies.lua"]
BOT_LUA = Path(__file__).resolve().parent / "bot" / "bot.lua"


def repo_fixed_overlay() -> str:
    """headless/out/lua-overlay/repo-fixed: the repo's fixed gameplay Lua (stem match through --lua-overlay)."""
    d = C.OUT / "lua-overlay" / "repo-fixed"
    d.mkdir(parents=True, exist_ok=True)
    for rel in REPO_FIXED_LUA:
        src = C.REPO / rel
        dst = d / src.name
        if not dst.is_file() or dst.read_bytes() != src.read_bytes():
            dst.write_bytes(src.read_bytes())
    return str(d.relative_to(C.REPO)).replace("\\", "/")


def bots_session(a) -> tuple[dict, Path]:
    """Build the bot-mode session: scenario copy with K human slots, N peers (peer i plays slot i), Lua bots on peers 0..K-1,
    exe turn model, masked digests, repo-fixed Lua, peer-0 inbox recording for the replay."""
    scen_path = Path(a.scenario)
    if not scen_path.is_absolute():
        scen_path = (C.REPO / scen_path) if (C.REPO / scen_path).is_file() else Path.cwd() / scen_path
    scen = C.read_json(scen_path)
    n, k = a.peers, a.humans
    if not (0 <= k <= n <= len(scen["civs"])):
        raise ValueError(f"need 0 <= humans ({k}) <= peers ({n}) <= civs ({len(scen['civs'])})")
    for i, civ in enumerate(scen["civs"]):
        civ.pop("human", None)
        if i < k:
            civ["human"] = True
    scen["game_type"] = "network"
    scen["game_options_preset"] = "lekmod-mp"
    scen["active_player"] = 0
    dll_name = Path(a.dll).name if a.dll else "v-release"
    name = a.name or f"bots-{scen_path.stem}-p{n}h{k}-s{a.seed}-t{a.turns}-ui{'1' if a.ui_queries == 'on' else '0'}-d{a.remote_delay}-{dll_name}"
    if not name.startswith("bots-"):
        name = "bots-" + name
    out = Path(a.out) if a.out else C.OUT / "sessions" / name
    out.mkdir(parents=True, exist_ok=True)
    scen["name"] = name
    scen_file = out / "scenario.json"
    C.write_json(scen_file, scen)
    bots = [{"peer": i, "lua": str(BOT_LUA), "seed": a.seed * 1000 + 7919 * i + 1, "density": a.density, "illegal": a.illegal,
             "ui_queries": a.ui_queries == "on", "end_turn_after": int(getattr(a, "fallback_end_turn", 0) or 0),
             "trade": float(getattr(a, "trade", 0.0) or 0.0),
             "trade_edit": float(getattr(a, "trade_edit", 0.0) or 0.0)} for i in range(k)]
    cfg = load_session(None, {"scenario": str(scen_file), "n_peers": n, "name": name, "until_turn": a.turns, "remote_delay": a.remote_delay})
    try:
        dll_path = _dll(a.dll or "v-release")
    except OSError:
        dll_path = None                                          # mask lines from the working tree; cmd_bots reports the DLL
    cfg.update(turn_model="exe", bots=bots, record_inbox=not a.no_replay, single=False,
               mask=expand_mask(["@bot"] if a.mask is None else a.mask, dll_path),
               lua_overlay=None if a.no_overlay else repo_fixed_overlay(), max_frames_per_turn=a.max_frames_per_turn,
               stop_on_divergence=not a.keep_going, dll=a.dll or "v-release", diag_dll=a.diag_dll, comment="python -m civsim.lockstep bots")
    if getattr(a, "dll_logs", False):
        cfg["extra_args"] = list(cfg.get("extra_args") or []) + ["--dll-logs"]   # every peer writes <peer>\Logs\ (LekmodNG-turns.log)
    ts = getattr(a, "turn_slices", "auto") or "auto"
    cfg["turn_slices"] = None if ts == "auto" else ts == "on"     # auto = on with a human slot (exe command order)
    return cfg, out


def cmd_bots(a) -> int:
    try:
        cfg, out = bots_session(a)
    except (OSError, ValueError, KeyError) as e:
        print(f"config: {e}", file=sys.stderr)
        return 2
    exe = C.resolve_host(a.host).path
    dll = _dll(cfg["dll"])
    diag = _dll(cfg.get("diag_dll"))
    until = int(cfg["until_turn"])
    print(f"lockstep bots {cfg['name']}: {len(cfg['peers'])} peers, {len(cfg['bots'])} human slot(s) played by bots, dll {dll.parent.name}, "
          f"until turn {until}, ui queries {a.ui_queries}, remote delay {cfg['remote_delay']}, out {out}")
    C.write_json(out / "session.json", cfg)
    s = Session(cfg, out, exe, dll)
    t0 = time.time()
    try:
        s.start()
        res = s.run(until)
    except PeerError as e:
        res = dict(s.result, status="error", error=str(e))
        print(f"ERROR: {e}", file=sys.stderr)
    finally:
        s.close()
    res.update(name=cfg["name"], session=cfg, dll=str(dll), diag_dll=str(diag) if diag else None, host=str(exe), wall_s=round(time.time() - t0, 1),
               peers=[{"id": i, "player": p["player"], "run_dir": str(out / f"peer-{i}"), "bot": i < len(cfg["bots"])} for i, p in enumerate(cfg["peers"])])
    if cfg.get("record_inbox") and (out / "inbox-peer0.jsonl").is_file() and res.get("status") in ("equal", "divergent", "hang"):
        try:
            res["replay_vs_peer0"] = run_replay(cfg, out, exe, dll, out / "inbox-peer0.jsonl")
        except (PeerError, OSError, ValueError) as e:
            res["replay_vs_peer0"] = {"error": str(e)}
            print(f"replay failed: {e}", file=sys.stderr)
    loc = None
    if res["status"] == "divergent" and not a.no_localize:
        try:
            loc = localize(cfg, res, out, exe, dll, diag, log=print)
            res["localize"] = loc
        except (PeerError, OSError, ValueError, KeyError) as e:
            res["localize_error"] = str(e)
            print(f"localize failed: {e}", file=sys.stderr)
    C.write_json(out / "result.json", res)
    if res["status"] == "divergent":
        C.write_json(out / "divergence.json", {"divergences": res["divergences"], "localize": loc})
        (out / "divergence.md").write_text(render_md(cfg, res, loc), encoding="utf-8")
    rp = res.get("replay_vs_peer0") or {}
    # the replay is judged like the peers: on the masked digest when a mask is set (the unmasked comparison stays in result.json)
    rp_key = "digest-masked.bin" if cfg.get("mask") and "digest-masked.bin" in rp else "digest.bin"
    rp_ok = bool((rp.get(rp_key) or {}).get("equal")) if rp and "error" not in rp and rp_key in rp else None
    print(f"result: {res['status']} after {res.get('frames')} frames, turns {res.get('turns')}, {res.get('wall_s')} s; replay==peer0: {rp_ok}; "
          f"messages {res.get('messages')}; bot actions {res.get('bot_actions')}; {out / 'result.json'}")
    rc = {"equal": 0, "divergent": 4, "hang": 5}.get(res["status"], 1)
    if rc == 0 and rp_ok is False:
        rc = 4
    return rc


def cmd_selftest(_a) -> int:
    fails = 0

    def check(cond: bool, what: str) -> None:
        nonlocal fails
        print(("ok   " if cond else "FAIL ") + what)
        fails += 0 if cond else 1

    msgs = [{"t": 5, "q": 3, "k": 1}, {"t": 5, "q": 3, "k": 0}, {"t": 4, "q": 9, "k": 2}, {"t": 5, "q": 1, "k": 3}]
    o = sorted(msgs, key=order_key)
    check(o[0]["t"] == 4 and o[1]["q"] == 1, "order: turn first, then seq")
    flip5 = (((5 * 0x41C64E6D + 0x3039) & 0xFFFFFFFF) >> 16) & 1
    check([m["k"] for m in o[2:]] == ([1, 0] if flip5 else [0, 1]), f"order: equal (turn, seq) -> sender direction bit {flip5}")
    check(sorted(reversed(msgs), key=order_key) == o, "order is independent of arrival order")
    cfg = load_session(None, {"scenario": "x.json", "n_peers": 3})
    check([p["player"] for p in cfg["peers"]] == [0, 1, 2] and cfg["game_type"] == "network" and cfg["game_options_preset"] == "lekmod-mp",
          "session defaults: network, lekmod-mp, players 0..N-1")
    check(diff_clusters(b"aaaaXbbbbbbbbbbbYc", b"aaaaZbbbbbbbbbbbWc") == [[4, 5], [16, 17]], "diff clusters")
    check(set(DEFAULTS) >= {"ai_sends", "turn_barrier", "remote_delay"}, "defaults carry the peer switches")
    check(DEFAULTS.get("turn_model") == "legacy", "AI-only sessions keep the legacy turn model unless they ask for exe")
    chunk = bot_init_chunk({"seed": 7, "density": 0.5, "illegal": 0.03, "ui_queries": True})
    check(chunk.startswith("HarnessBotConfig = {") and "ui_queries = true" in chunk and "seed = 7.0" in chunk, "bot init chunk")
    check(BOT_LUA.is_file() and "function HarnessBotTick" in BOT_LUA.read_text(encoding="utf-8"), "bot.lua present with HarnessBotTick")
    ba = argparse.Namespace(scenario="headless/scenarios/smoke/lekmod6.json", peers=3, humans=2, seed=5, dll="v-release", turns=10, ui_queries="off",
                            remote_delay=0, density=1.0, illegal=0.03, name="selftest-bots", out=str(C.OUT / "sessions" / "bots-selftest"),
                            no_replay=False, mask=None, no_overlay=True, max_frames_per_turn=8000, keep_going=False, diag_dll=None)
    bcfg, bout = bots_session(ba)
    bscen = C.read_json(bout / "scenario.json")
    check([bool(c.get("human")) for c in bscen["civs"]][:3] == [True, True, False] and bcfg["turn_model"] == "exe"
          and [b["peer"] for b in bcfg["bots"]] == [0, 1] and len({b["seed"] for b in bcfg["bots"]}) == 2 and bcfg["record_inbox"],
          "bots session: K human slots, bots on peers 0..K-1 with distinct seeds, exe turn model, replay recording")
    tm = tree_mask(None)
    src_game = (C.REPO / DLL_SRC / "CvGame.cpp").read_text(encoding="utf-8", errors="replace").splitlines()
    tl = int(tm[3].split(":")[1].rstrip("|"))
    check(len(tm) == 4 and tm[0] == "cvunitcycler.cpp" and tm[3].startswith("cvgame.cpp:")
          and src_game[tl - 1].strip() == "kStream << m_bTutorialEverAttacked;", f"tree masks derived from the source: {tm}")
    check(bcfg["mask"] == tree_mask(_dll("v-release")) + BOT_EXTRA_MASK and expand_mask(["@tree", "x.cpp"], None) == tm + ["x.cpp"],
          "bot masks = the DLL's tree masks + notifications / diplomacy requests; '@tree' expands")
    check(turn_slices_on(bcfg) and not turn_slices_on(dict(bcfg, bots=[])) and not turn_slices_on(dict(bcfg, turn_slices=False))
          and not turn_slices_on(load_session(None, {"scenario": "x.json"})),
          "turn slices: auto = on for exe turns with a human slot, off for AI-only / legacy sessions, explicit off honoured")
    print(f"selftest: {fails} failure(s)")
    return 1 if fails else 0


def cmd_localize(a) -> int:
    """Rerun the localization of a finished divergent session (headless/out/sessions/<name>/result.json)."""
    out = Path(a.dir) if Path(a.dir).is_dir() else C.OUT / "sessions" / a.dir
    res = C.read_json(out / "result.json")
    if res.get("status") != "divergent":
        print(f"{out}: status {res.get('status')}, nothing to localize", file=sys.stderr)
        return 2
    cfg = res["session"]
    exe = C.resolve_host(a.host).path
    dll = Path(res["dll"])
    diag = _dll(a.diag_dll) if a.diag_dll else (Path(res["diag_dll"]) if res.get("diag_dll") else None)
    loc = localize(cfg, res, out, exe, dll, diag, log=print)
    res["localize"] = loc
    res.pop("localize_error", None)
    C.write_json(out / "result.json", res)
    C.write_json(out / "divergence.json", {"divergences": res["divergences"], "localize": loc})
    (out / "divergence.md").write_text(render_md(cfg, res, loc), encoding="utf-8")
    print(f"localize: segments {loc['segments']}, reproduced {loc['reproduced']}; {out / 'divergence.md'}")
    return 4


def cmd_smoke(a) -> int:
    """Integration check (~1 min): peer mode N=1 equals the single-process loopback run, and 2 masked spectator peers stay equal.
    Uses the session file (lekmod6, its build-specific mask lines) of the chosen DLL build."""
    session = Path(__file__).resolve().parents[3] / "scenarios" / "lockstep" / a.session
    base = [str(session), "--until-turn", str(a.turns), "--no-localize"]
    if a.dll:
        base += ["--dll", a.dll]
    if a.host:
        base += ["--host", a.host]
    rc1 = main(["run", *base, "--peers", "1", "--name", "smoke-lockstep-p1"])
    r1 = C.read_json(C.OUT / "sessions" / "smoke-lockstep-p1" / "result.json")
    ok1 = rc1 == 0 and (r1.get("single_vs_peer0") or {}).get("identical")
    rc2 = main(["run", *base, "--peers", "2", "--name", "smoke-lockstep-p2", "--no-single"] + sum((["--mask", m] for m in a.mask or []), []))
    print(f"smoke: N=1 == single: {bool(ok1)}; 2 peers: exit {rc2} ({'equal' if rc2 == 0 else 'see result.json'})")
    return 0 if ok1 and rc2 == 0 else 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="python -m civsim.lockstep")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run a lockstep session")
    r.add_argument("session", nargs="?", help="session JSON (headless/scenarios/lockstep/*.json)")
    r.add_argument("--scenario", help="scenario JSON (instead of / overriding the session's)")
    r.add_argument("--peers", type=int, help="number of peers (players 0..N-1 unless the session lists them)")
    r.add_argument("--dll", help="DLL variant dir name under headless/out/dll or a path (default golden)")
    r.add_argument("--diag-dll", help="diag build of the same flag set for the localization rerun (RNG call log)")
    r.add_argument("--until-turn", type=int)
    r.add_argument("--ai-sends", choices=["all", "owner"])
    r.add_argument("--order", choices=["ordered", "peer"])
    r.add_argument("--remote-delay", type=int, help="extra frames before another peer receives a message (the sender gets it next frame)")
    r.add_argument("--turn-barrier", action="store_true", help="CanAdvanceTurn waits until every peer asked for the same turn")
    r.add_argument("--lua-overlay")
    r.add_argument("--name")
    r.add_argument("--out")
    r.add_argument("--host")
    r.add_argument("--no-single", action="store_true", help="skip the single-process reference run")
    r.add_argument("--no-localize", action="store_true")
    r.add_argument("--keep-going", action="store_true", help="do not stop at the first divergence")
    r.add_argument("--mask", action="append", help="extra mask pattern (function / file / 'file.cpp:LINE|' of this DLL build); repeatable; "
                                                   "'@tree' = the DLL's unit cycler / world anchor / tutorial latch lines, '@bot' = those + bot extras")
    r.add_argument("--no-mask", action="store_true", help="compare the unmasked digests")
    r.set_defaults(fn=cmd_run)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    lo = sub.add_parser("localize", help="rerun the localization of a divergent session")
    lo.add_argument("dir", help="session name or directory")
    lo.add_argument("--diag-dll")
    lo.add_argument("--host")
    lo.set_defaults(fn=cmd_localize)
    b = sub.add_parser("bots", help="human slots played by seeded bots (bot mode)")
    b.add_argument("--peers", type=int, default=2, help="number of peers N (peer i plays slot i)")
    b.add_argument("--humans", type=int, default=1, help="human slots K (slots 0..K-1, each played by the bot of its own peer)")
    b.add_argument("--seed", type=int, default=1, help="bot seed (host-side PRNG; peer i uses seed*1000+7919*i+1)")
    b.add_argument("--dll", default=None, help="DLL variant (default v-release)")
    b.add_argument("--diag-dll", default=None)
    b.add_argument("--scenario", default="headless/scenarios/smoke/lekmod6.json")
    b.add_argument("--turns", type=int, default=60)
    b.add_argument("--ui-queries", choices=["on", "off"], default="off", help="the bot also calls read-only UI getters on its own peer")
    b.add_argument("--remote-delay", type=int, default=0)
    b.add_argument("--turn-slices", choices=["auto", "on", "off"], default="auto",
                   help="the exe's command order: a peer's commands leave it only in its own NetTurnSlice (auto = on with a human slot; "
                        "off = the old model, every command broadcast in the frame it was issued)")
    b.add_argument("--density", type=float, default=1.0, help="actions per turn = density * (units + cities + 3)")
    b.add_argument("--illegal", type=float, default=0.03, help="share of deliberately illegal / stale commands")
    b.add_argument("--trade", type=float, default=0.0, help="per-frame chance that a bot works the trade screen with an AI major it has "
                   "met (propose / accept / decline / close through UI.GetScratchDeal, UI.DoProposeDeal, Player:DoTradeScreenOpened/Closed); "
                   "0 = off (bot PRNG stream as before). Needs a civhost with the trade-screen bindings")
    b.add_argument("--trade-edit", type=float, default=0.0, help="with --trade: per trade-screen step, chance to retype the gold / "
                   "gold-per-turn amount boxes (Deal:ChangeGoldPerTurnTrade / ChangeGoldTrade, as TradeLogic's amount handlers); "
                   "0 = off (bot PRNG stream as with --trade alone)")
    b.add_argument("--mask", action="append", default=None, help="replace the default mask lines ('@bot' = the default: the DLL's "
                                                                 "tree lines + notifications / diplomacy requests; '@tree' = the tree lines only)")
    b.add_argument("--no-overlay", action="store_true", help="installed Lekmod Lua instead of the repo-fixed overlay")
    b.add_argument("--no-replay", action="store_true", help="skip the single-process replay of peer 0")
    b.add_argument("--no-localize", action="store_true")
    b.add_argument("--keep-going", action="store_true")
    b.add_argument("--max-frames-per-turn", type=int, default=8000)
    b.add_argument("--dll-logs", action="store_true", help="civhost --dll-logs on every peer (DLL logs incl. Logs\\LekmodNG-turns.log)")
    b.add_argument("--fallback-end-turn", type=int, default=0,
                   help="frames into a turn before the coordinator ends a bot's turn for it (0 = as soon as all AI civs are processed, "
                        "which leaves the bot about one frame per turn; e.g. 300 lets the bot play and end its own turn)")
    b.add_argument("--name")
    b.add_argument("--out")
    b.add_argument("--host")
    b.set_defaults(fn=cmd_bots)
    s = sub.add_parser("smoke", help="integration check: N=1 == single, 2 peers equal")
    s.add_argument("--session", default="lek6-spectator-ghyg.json", help="session file under headless/scenarios/lockstep (its dll and masks)")
    s.add_argument("--dll", help="override the session's DLL (then pass that build's mask lines with --mask)")
    s.add_argument("--turns", type=int, default=5)
    s.add_argument("--host")
    s.add_argument("--mask", action="append", help="extra build-specific mask lines")
    s.set_defaults(fn=cmd_smoke)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
