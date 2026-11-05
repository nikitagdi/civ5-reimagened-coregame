"""Lockstep coordinator: N civhost peers (control channel, peer mode), deterministic frames, per-turn digest comparison.

Frame f: every peer gets its inbox (messages emitted in frame f-1, or earlier with --remote-delay), executes it through
ICvNetMessageHandler (the single-mode loopback path), runs one ICvGame1::Update slice and returns its outbox. Inbox order is
the exe's FNetMessageQueue_InsertOrdered key (turn, seq, sender) with the per-turn sender direction bit (02 §2.6), so every
peer executes the same messages in the same order regardless of process scheduling.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from .. import common as C
from .. import digestbin as D
from .peer import Peer, PeerError

DEFAULTS: dict[str, Any] = {
    "scenario": None, "game_type": "network", "game_options_preset": "lekmod-mp", "until_turn": 50, "peers": 2, "host": 0,
    "ai_sends": "all", "turn_barrier": False, "order": "ordered", "remote_delay": 0, "digest_every": 1, "max_frames_per_turn": 4000,
    "lua_overlay": None, "script": [], "bots": [], "single": True, "extra_args": [], "stop_on_divergence": True,
    # turn_model "exe": the exe's MP turn handshake in every peer (human turns); "legacy" (default) keeps the AI-only behaviour
    # of the earlier sessions (CanAdvanceTurn true, no NetTurnAllComplete / NetLoopBarrier).
    "turn_model": "legacy", "record_inbox": False,
    # turn_slices: the exe's command order (send queue + NetTurnSlice token, exe 0x3fc4b0 / 0x307d0 / 0x129600): a peer's game
    # commands leave it only in its own turn slice. None = on with the exe turn model when the session has a human (a bot).
    "turn_slices": None,
    # mask-only state left out of the compared (masked) digest: writes whose call chain hits these function/file substrings
    "mask": ["cvunitcycler.cpp"],
}


def order_key(m: dict) -> tuple:
    """exe FNetMessageQueue_InsertOrdered 0x3ff720: (turn, seq, sender); for equal turn and seq the sender direction flips with
    bit 16 of the LCG of the turn."""
    t, q, k = m.get("t", 0), m.get("q", 0), m.get("k", 0)
    flip = (((t * 0x41C64E6D + 0x3039) & 0xFFFFFFFF) >> 16) & 1
    return (t, q, -k if flip else k)


def turn_slices_on(cfg: dict) -> bool:
    """Effective turn_slices: explicit true/false, else on for the exe turn model with at least one human slot (bot)."""
    v = cfg.get("turn_slices")
    if v is None:
        return (cfg.get("turn_model") or "legacy") == "exe" and bool(cfg.get("bots"))
    return bool(v)


def load_session(path: Path | None, overrides: dict) -> dict:
    cfg = dict(DEFAULTS)
    if path:
        cfg.update(C.read_json(path))
        cfg.setdefault("name", path.stem)
    for k, v in overrides.items():
        if v is not None:
            cfg[k] = v
    peers = cfg["peers"]
    if isinstance(peers, int):
        peers = [{"player": i} for i in range(peers)]
    peers = [p if isinstance(p, dict) else {"player": p} for p in peers]
    if overrides.get("n_peers"):
        n = overrides["n_peers"]
        peers = (peers + [{"player": i} for i in range(len(peers), n)])[:n]
    cfg["peers"] = peers
    if not cfg.get("scenario") and not cfg.get("import_save"):
        raise ValueError("session needs a scenario (or import_save)")
    if cfg.get("import_save") and cfg.get("single", True):
        cfg["single"] = False           # the single-process reference starts from a scenario; an imported session has none
    cfg.setdefault("name", Path(cfg.get("scenario") or "import").stem + "-lockstep")
    return cfg


def import_dirs(cfg: dict) -> list[str] | None:
    """import_save: one extracted save dir (python -m civsim.saveio extract) for every peer, or a list with one dir per peer
    (e.g. the same body with each peer's own active player in pregame.bin). None = a new game from the scenario."""
    imp = cfg.get("import_save")
    if not imp:
        return None
    n = len(cfg["peers"])
    dirs = [imp] * n if isinstance(imp, str) else list(imp)
    if len(dirs) != n:
        raise ValueError(f"import_save lists {len(dirs)} dirs for {n} peers")
    return [str(C.REPO / d) if not Path(d).is_absolute() else d for d in dirs]


class Session:
    def __init__(self, cfg: dict, out: Path, exe: Path, dll: Path, log=print, dump_state_at: int = -1, extra_peer_args: list[str] | None = None):
        self.cfg, self.out, self.exe, self.dll, self.log = cfg, out, exe, dll, log
        self.dump_state_at = dump_state_at
        self.extra_peer_args = list(extra_peer_args or [])
        self.peers: list[Peer] = []
        self.events = None
        self.frame = 0
        self.msg_counts: Counter = Counter()
        self.turn_frames: dict[int, dict[int, int]] = defaultdict(dict)    # turn -> peer -> frame of the turn change
        self.result: dict[str, Any] = {}

    def ev(self, kind: str, **kw) -> None:
        if self.events:
            kw = {"ev": kind, "frame": self.frame, **kw}
            self.events.write(json.dumps(kw, separators=(",", ":")) + "\n")

    # ------------------------------------------------------------------ start
    def start(self) -> list[dict]:
        cfg = self.cfg
        self.out.mkdir(parents=True, exist_ok=True)
        self.events = open(self.out / "events.jsonl", "w", encoding="utf-8")
        extra = list(cfg.get("extra_args") or []) + self.extra_peer_args
        if cfg.get("lua_overlay"):
            extra += ["--lua-overlay", str(C.REPO / cfg["lua_overlay"])]
        players = [p["player"] for p in cfg["peers"]]
        for i, p in enumerate(cfg["peers"]):
            d = self.out / f"peer-{i}"
            if d.exists():
                shutil.rmtree(d, ignore_errors=True)
            self.peers.append(Peer(i, p["player"], d, self.exe, self.dll, extra))
        n = len(self.peers)
        int_players = [x for x in players if isinstance(x, int)]
        for p in self.peers:
            p.send("hello")
        hellos = [p.read() for p in self.peers]
        for p in self.peers:
            p.send("peer", id=p.id, peers=n, host=cfg["host"], ai_sends=cfg["ai_sends"], turn_barrier=bool(cfg["turn_barrier"]), players=int_players,
                   mask=list(cfg.get("mask") or []), turn_model=cfg.get("turn_model") or "legacy", turn_slices=turn_slices_on(cfg))
        for p in self.peers:
            p.read()
        imports = import_dirs(cfg)
        if imports:
            # a real save: game type, options and the active player come from each dir's pregame.bin (civhost import_save)
            for p, d in zip(self.peers, imports):
                p.send("import_save", dir=d, keep_humans=bool(cfg.get("keep_humans", True)))
        else:
            scen = str(C.REPO / cfg["scenario"]) if not Path(cfg["scenario"]).is_absolute() else cfg["scenario"]
            for p in self.peers:
                kw = {"scenario": scen, "active_player": p.player, "game_type": cfg["game_type"]}
                if cfg.get("game_options_preset"):
                    kw["game_options_preset"] = cfg["game_options_preset"]
                p.send("new_game", **kw)
        games = [p.read() for p in self.peers]
        if imports:
            for p, g in zip(self.peers, games):
                if not g.get("ok", True):
                    raise PeerError(f"peer {p.id}: import_save failed: {g}")
                if g.get("active_player") is not None and g["active_player"] != p.player:     # if civhost reports it
                    raise PeerError(f"peer {p.id}: imported save has active player {g['active_player']}, the session gives player {p.player}")
        for p, g in zip(self.peers, games):
            p.turn = g["turn"]
            self.ev("new_game", peer=p.id, player=p.player, turn=g["turn"], map_digest=g["map_digest"], start_digest=g["start_digest"],
                    lua_errors=g["lua_errors"], contexts=g["lua_contexts_loaded"])
        # Lua bots (bot mode): the local UI of a human slot, loaded on its own peer only
        for b in cfg.get("bots") or []:
            if not b.get("lua"):
                continue
            p = self.peers[b["peer"]]
            p.call("bot", file=str(Path(b["lua"]).resolve()), init=bot_init_chunk(b))
            p.lua_bot = True
            self.ev("bot_loaded", peer=p.id, player=p.player, seed=b.get("seed"), density=b.get("density"), ui_queries=b.get("ui_queries"))
        self.result["hello"] = {"host": hellos[0]["host"], "dll": hellos[0]["dll"], "dll_xxh64": hellos[0]["dll_xxh64"]}
        self.result["start"] = [{"peer": p.id, "player": p.player, "turn": g["turn"], "map_digest": g["map_digest"], "start_digest": g["start_digest"],
                                 "lua_contexts_loaded": g["lua_contexts_loaded"], "lua_errors": g["lua_errors"]} for p, g in zip(self.peers, games)]
        return games

    # ------------------------------------------------------------------ frames
    def run(self, until_turn: int) -> dict:
        cfg = self.cfg
        n = len(self.peers)
        pending: list[dict[int, list]] = [defaultdict(list) for _ in range(n)]
        delay = int(cfg.get("remote_delay") or 0)
        grant = -1
        compared: set[int] = set()
        divergences: list[dict] = []
        mask_only: list[int] = []      # turns where only the unmasked digests differ (mask-only state)
        script = sorted(cfg.get("script") or [], key=lambda s: (s["turn"], s.get("peer", 0)))
        done_script: set[int] = set()
        bots = {b["peer"]: b for b in (cfg.get("bots") or [])}
        last_change_frame = 0
        status = "running"
        t0 = time.time()
        lua_errors = [0] * n
        inbox_log = open(self.out / "inbox-peer0.jsonl", "w", encoding="utf-8") if cfg.get("record_inbox") else None
        bot_counts: Counter = Counter()
        while True:
            # scripted bot commands (before the frame, as if the local UI issued them)
            for idx, s in enumerate(script):
                if idx in done_script:
                    continue
                p = self.peers[s.get("peer", 0)]
                if p.turn is not None and p.turn >= s["turn"] and p.frames_in_turn >= int(s.get("frame_in_turn", 0)):
                    cmd = s.get("cmd", "inject")
                    kw = {k: v for k, v in s.items() if k in ("method", "args", "text", "from")}
                    r = p.call(cmd, **kw)
                    done_script.add(idx)
                    self.ev("script", peer=p.id, turn=p.turn, cmd=cmd, **kw)
                    self.log(f"  frame {self.frame}: script peer {p.id} turn {p.turn}: {cmd} {kw}")
            for i, p in enumerate(self.peers):
                inbox = sorted(pending[i].pop(self.frame, []), key=order_key) if cfg["order"] == "ordered" else pending[i].pop(self.frame, [])
                kw: dict[str, Any] = {"inbox": inbox, "digest_every": cfg["digest_every"]}
                if grant >= 0:
                    kw["grant"] = grant
                if self.dump_state_at >= 0:
                    kw["dump_state_at"] = self.dump_state_at
                if getattr(p, "lua_bot", False):
                    kw["bot"] = "play"
                if inbox_log is not None and i == 0:
                    inbox_log.write(json.dumps({"f": self.frame, "inbox": inbox}, separators=(",", ":")) + "\n")
                    inbox_log.flush()       # a session killed in a hang keeps the inbox of the frame that hung (replay to the hang)
                p.send("step", **kw)
            replies = [p.read() for p in self.peers]
            frame = self.frame
            self.frame += 1
            wants = set()
            for i, (p, r) in enumerate(zip(self.peers, replies)):
                p.last = r
                lua_errors[i] = r.get("lua_errors", 0)
                out = r.get("outbox") or []
                if out:
                    self.ev("send", peer=p.id, turn=r["from_turn"], msgs=out)
                for m in out:
                    self.msg_counts[m["m"]] += 1
                    for j in range(n):
                        pending[j][self.frame + (0 if j == i else delay)].append(m)
                p.frames_in_turn += 1
                if r.get("bot"):
                    self.ev("bot", peer=p.id, turn=r["from_turn"], a=r["bot"])
                    for a in r["bot"].split(","):
                        bot_counts[a.split(":")[0]] += 1
                if r["turn_changed"]:
                    p.turn = r["turn"]
                    p.frames_in_turn = 0
                    self.turn_frames[r["turn"]][p.id] = frame
                    if "digest" in r:
                        p.digests[r["turn"]] = r.get("mdigest", r["digest"])     # compare the masked digest when a mask is set
                        p.raw_digests[r["turn"]] = r["digest"]
                    self.ev("turn", peer=p.id, turn=r["turn"], frames=r.get("frames"), digest=r.get("digest"), mdigest=r.get("mdigest"),
                            masked_bytes=r.get("masked_bytes"))
                    last_change_frame = frame
                wants.add(r.get("want_advance", -1))
                b = bots.get(p.id)
                # end_turn_after (bot mode --fallback-end-turn F > 0): a Lua bot ends its own turn - its Game.DoControl(CONTROL_ENDTURN)
                # ("end-turn" in its frame log) is turned into the local sendTurnComplete here, because civhost never calls
                # ICvGame1::UpdateTestEndTurn, so the UI's canEndTurn() stays false and DoControl does nothing; the coordinator
                # ends the turn itself only after F frames of the turn. 0 (default) = as soon as all AI civs are processed.
                if b and b.get("auto_end_turn", True) and r.get("all_ai") and not r.get("tc_sent"):
                    after = int(b.get("end_turn_after") or 0)
                    asked = bool(b.get("lua")) and after > 0 and any(x in ("end-turn", "force-end-turn") for x in (r.get("bot") or "").split(","))
                    if asked or p.frames_in_turn >= after:
                        p.call("end_turn")
                        self.ev("bot_end_turn", peer=p.id, turn=r["turn"], by="bot" if asked else "fallback")
            if len(wants) == 1 and min(wants) >= 0 and min(wants) != grant:
                grant = min(wants)
                self.ev("grant", turn=grant)
            # per-turn comparison once every peer reported the turn
            for t in sorted(set.intersection(*[set(p.digests) for p in self.peers])) if self.peers else []:
                if t in compared:
                    continue
                compared.add(t)
                groups: dict[str, list[int]] = defaultdict(list)
                for p in self.peers:
                    groups[p.digests[t]].append(p.id)
                frames = self.turn_frames.get(t, {})
                if len(groups) > 1:
                    div = {"turn": t, "groups": [{"digest": d, "peers": ps, "players": [self.peers[x].player for x in ps]} for d, ps in groups.items()],
                           "turn_change_frames": frames}
                    divergences.append(div)
                    self.ev("desync", **div)
                    self.log(f"  DIVERGENCE at turn {t}: " + " vs ".join(f"peers {g['peers']} {g['digest']}" for g in div["groups"]))
                else:
                    if len(set(frames.values())) > 1:
                        self.ev("timing", turn=t, turn_change_frames=frames)
                    if len({p.raw_digests.get(t) for p in self.peers}) > 1:
                        mask_only.append(t)
                if t % 10 == 0:
                    self.log(f"  turn {t}: {len(self.peers)} peers equal ({time.time() - t0:.0f} s, frame {self.frame})")
            if divergences and cfg.get("stop_on_divergence", True):
                status = "divergent"
                break
            if all(p.turn is not None and p.turn >= until_turn for p in self.peers):
                status = "divergent" if divergences else "equal"
                break
            if self.frame - last_change_frame > int(cfg["max_frames_per_turn"]):
                status = "hang"
                self.ev("hang", turns=[p.turn for p in self.peers], last=[p.last for p in self.peers])
                self.log(f"  HANG: no turn change for {self.frame - last_change_frame} frames; peers at turns {[p.turn for p in self.peers]}")
                break
        if inbox_log is not None:
            inbox_log.close()
        if bot_counts:
            self.result["bot_actions"] = dict(bot_counts.most_common())
        self.result.update(status=status, frames=self.frame, seconds=round(time.time() - t0, 1), divergences=divergences,
                           turns=[p.turn for p in self.peers], messages=dict(self.msg_counts), lua_errors=lua_errors,
                           compared_turns=len(compared), mask=list(cfg.get("mask") or []),
                           mask_only_turns={"first": mask_only[0], "last": mask_only[-1], "count": len(mask_only)} if mask_only else None)
        return self.result

    def close(self) -> None:
        for p in self.peers:
            p.send("end_game") if p.proc.poll() is None else None
        for p in self.peers:
            if p.proc.poll() is None:
                try:
                    p.read()
                except PeerError:
                    pass
        for p in self.peers:
            p.close()
        if self.events:
            self.events.close()
            self.events = None


def bot_init_chunk(b: dict) -> str:
    """Lua chunk run before bot.lua: HarnessBotConfig = {...} (numbers / booleans only)."""
    items = []
    for k in ("seed", "density", "illegal", "stuck_frames", "trade", "trade_edit"):
        if b.get(k) is not None:
            items.append(f"{k} = {float(b[k])!r}")
    items.append(f"ui_queries = {'true' if b.get('ui_queries') else 'false'}")
    return "HarnessBotConfig = { " + ", ".join(items) + " }"


def run_replay(cfg: dict, out: Path, exe: Path, dll: Path, inbox_file: Path, log=print, extra_peer_args: list[str] | None = None) -> dict:
    """Single-process replay of peer 0: one civhost with peer 0's configuration (id 0 of N, same players, turn model, mask) and no
    bot, fed exactly the inbox peer 0 received in every frame (the whole command stream of all peers, bots included). Its own
    outbox is discarded (its messages are in the recorded stream). The digests must equal peer 0's."""
    d = out / "replay"
    if d.exists():
        shutil.rmtree(d, ignore_errors=True)
    extra = list(cfg.get("extra_args") or []) + list(extra_peer_args or [])
    if cfg.get("lua_overlay"):
        extra += ["--lua-overlay", str(C.REPO / cfg["lua_overlay"])]
    p0 = cfg["peers"][0]["player"]
    peer = Peer(0, p0, d, exe, dll, extra)
    n = len(cfg["peers"])
    players = [x["player"] for x in cfg["peers"] if isinstance(x["player"], int)]
    t0 = time.time()
    try:
        peer.call("hello")
        peer.call("peer", id=0, peers=n, host=cfg["host"], ai_sends=cfg["ai_sends"], turn_barrier=bool(cfg["turn_barrier"]), players=players,
                  mask=list(cfg.get("mask") or []), turn_model=cfg.get("turn_model") or "legacy", turn_slices=turn_slices_on(cfg))
        scen = str(C.REPO / cfg["scenario"]) if not Path(cfg["scenario"]).is_absolute() else cfg["scenario"]
        kw = {"scenario": scen, "active_player": p0, "game_type": cfg["game_type"]}
        if cfg.get("game_options_preset"):
            kw["game_options_preset"] = cfg["game_options_preset"]
        peer.call("new_game", **kw)
        frames = 0
        with open(inbox_file, encoding="utf-8") as f:
            for line in f:
                rec = json.loads(line)
                peer.call("step", inbox=rec["inbox"], digest_every=cfg["digest_every"])
                frames += 1
        peer.call("end_game")
    finally:
        peer.close()
    res: dict = {"frames": frames, "seconds": round(time.time() - t0, 1), "run_dir": str(d)}
    for name in ("digest.bin", "digest-masked.bin"):
        a, b = d / name, out / "peer-0" / name
        if a.is_file() and b.is_file():
            cmp = D.compare(D.parse(a), D.parse(b))
            res[name] = {k: cmp[k] for k in ("equal", "identical", "common_turns", "first_divergent_turn", "divergent_segments", "n_divergent_turns")}
    log(f"replay of peer 0 ({frames} frames): " + ", ".join(f"{k} {'equal' if v.get('equal') else 'DIFFERS from turn %s' % v.get('first_divergent_turn')} "
                                                         f"({v.get('common_turns')} turns)" for k, v in res.items() if isinstance(v, dict)))
    return res


def run_single(cfg: dict, out: Path, exe: Path, dll: Path, until_turn: int, extra: list[str] | None = None) -> subprocess.Popen:
    """The single-process reference: same scenario, game type and options, active player = peer 0's player, loopback net."""
    if out.exists():
        shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    args = [str(exe), "--dll", str(dll), "--scenario", str(C.REPO / cfg["scenario"]), "--until-turn", str(until_turn), "--out", str(out),
            "--game-type", cfg["game_type"], "--active-player", str(cfg["peers"][0]["player"])]
    if cfg.get("game_options_preset"):
        args += ["--game-options-preset", cfg["game_options_preset"]]
    if cfg.get("lua_overlay"):
        args += ["--lua-overlay", str(C.REPO / cfg["lua_overlay"])]
    args += list(cfg.get("extra_args") or []) + list(extra or [])
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(out / "stderr.txt", "wb"), cwd=str(C.REPO))
    C.attach_process(p)
    return p


def segment_diff(a: Path, b: Path) -> dict:
    da, db = D.parse(a), D.parse(b)
    return D.compare(da, db)
