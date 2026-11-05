"""Localize a cross-peer divergence: segments (digest.bin), bytes and writing source line (rerun with --dump-state-at and
--trace-writes), first differing RNG call (rerun with --rng-digest / --rng-log on a diag DLL). Reuses civsim.detlab helpers."""
from __future__ import annotations

import re
import struct
from pathlib import Path
from typing import Any

from .. import digestbin as D
from .. import rngbin as RB
from ..detlab import dyn as DY
from .coord import Session

_HELPER_FILES = re.compile(r"(FDataStream|StreamSaveLoad|FFastVector|FStlContainer|FAutoVariable|FAutoArchive|FSerializ|Serializ|"
                           r"CvEnumSerialization|xstring|vector|FString|FFixedVector|FStaticVector|algorithm|xutility)", re.I)


def parse_trace(path: Path) -> list[tuple[int, int, int, tuple[int, ...]]]:
    """<segment>.trace: u32 n, n x {u32 offset, u32 size, u32 calls, u32 ret[4]}."""
    data = path.read_bytes()
    (n,) = struct.unpack_from("<I", data, 0)
    out = []
    for i in range(n):
        off, size, calls, r0, r1, r2, r3 = struct.unpack_from("<7I", data, 4 + 28 * i)
        out.append((off, size, calls, (r0, r1, r2, r3)))
    return out


def parse_sites(path: Path) -> dict[int, dict]:
    sites = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) >= 5:
            sites[int(parts[0], 16)] = {"function": parts[2], "file": parts[3], "line": parts[4]}
    return sites


def attribute(trace: list, sites: dict[int, dict], off: int) -> dict | None:
    for (o, size, calls, rets) in trace:
        if o <= off < o + max(size, 1):
            frames = [sites.get(r, {"function": f"0x{r:08x}", "file": "", "line": "0"}) for r in rets if r]
            pick = next((f for f in frames if f["file"] and not _HELPER_FILES.search(Path(f["file"]).name)), frames[0] if frames else None)
            return {"range": [o, size], "site": pick, "stack": [f"{f['function']} {Path(f['file']).name}:{f['line']}" for f in frames]}
    return None


def diff_clusters(a: bytes, b: bytes, gap: int = 8, limit: int = 40) -> list[list[int]]:
    """[start, end) byte ranges where a and b differ (over the common length), merged across gaps < gap."""
    n = min(len(a), len(b))
    out: list[list[int]] = []
    i = 0
    # fast skip of equal 4 KB blocks
    while i < n and len(out) < limit:
        j = min(n, i + 4096)
        if a[i:j] == b[i:j]:
            i = j
            continue
        while i < j:
            if a[i] != b[i]:
                if out and i - out[-1][1] < gap:
                    out[-1][1] = i + 1
                else:
                    out.append([i, i + 1])
                    if len(out) >= limit:
                        break
            i += 1
    return out


def localize(cfg: dict, res: dict, out: Path, exe: Path, dll: Path, diag_dll: Path | None, log=print) -> dict:
    div = res["divergences"][0]
    t = div["turn"]
    pa, pb = div["groups"][0]["peers"][0], div["groups"][1]["peers"][0]
    loc: dict[str, Any] = {"turn": t, "peer_a": pa, "peer_b": pb, "notes": []}
    masked = bool(cfg.get("mask")) and (out / f"peer-{pa}" / "digest-masked.bin").is_file()
    dname = "digest-masked.bin" if masked else "digest.bin"
    loc["digest_file"] = dname
    cmp1 = D.compare(D.parse(out / f"peer-{pa}" / dname), D.parse(out / f"peer-{pb}" / dname))
    loc["segments"] = cmp1["divergent_segments"]
    loc["n_divergent_turns_seen"] = cmp1["n_divergent_turns"]
    rdll = diag_dll or dll
    rng = diag_dll is not None
    flags = ["--trace-writes"] + (["--rng-digest", "--rng-log", f"{max(t - 1, 0)}-{max(t - 1, 0)}"] if rng else [])
    sub_cfg = dict(cfg, stop_on_divergence=True, single=False)
    rdir = out / "localize"
    log(f"localize: rerun to turn {t} with state dump{' + RNG log (' + rdll.parent.name + ')' if rng else ''}")
    s = Session(sub_cfg, rdir, exe, rdll, log=lambda m: None, dump_state_at=t, extra_peer_args=flags)
    try:
        s.start()
        r2 = s.run(t)
    finally:
        s.close()
    d2 = r2["divergences"][0] if r2["divergences"] else None
    loc["rerun"] = {"dll": str(rdll), "status": r2["status"], "turn": d2["turn"] if d2 else None,
                    "groups": [g["peers"] for g in d2["groups"]] if d2 else None}
    loc["reproduced"] = bool(d2 and d2["turn"] == t and sorted(map(sorted, (g["peers"] for g in d2["groups"]))) == sorted(map(sorted, (g["peers"] for g in div["groups"]))))
    if not loc["reproduced"]:
        loc["notes"].append("the rerun did not reproduce the same divergence (turn/groups); byte level below is from the rerun state")
    sfx = "-masked" if masked else ""
    da, db = rdir / f"peer-{pa}" / f"state-{t}{sfx}", rdir / f"peer-{pb}" / f"state-{t}{sfx}"
    sites_a = parse_sites(da / "sites.tsv") if (da / "sites.tsv").is_file() else {}
    sites_b = parse_sites(db / "sites.tsv") if (db / "sites.tsv").is_file() else {}
    segs = []
    for seg in loc["segments"]:
        fa, fb = da / f"{seg}.bin", db / f"{seg}.bin"
        if not (fa.is_file() and fb.is_file()):
            segs.append({"name": seg, "error": "no state dump"})
            continue
        ba, bb = fa.read_bytes(), fb.read_bytes()
        e: dict[str, Any] = {"name": seg, **DY.byte_diff(ba, bb)}
        clusters = diff_clusters(ba, bb)
        e["clusters"] = []
        ta = parse_trace(da / f"{seg}.trace") if (da / f"{seg}.trace").is_file() else []
        tb = parse_trace(db / f"{seg}.trace") if (db / f"{seg}.trace").is_file() else []
        for c in clusters[:12]:
            item: dict[str, Any] = {"start": c[0], "end": c[1], "a": ba[c[0]:min(c[1], c[0] + 16)].hex(" "), "b": bb[c[0]:min(c[1], c[0] + 16)].hex(" ")}
            if ta:
                item["writer_a"] = attribute(ta, sites_a, c[0])
            if tb:
                item["writer_b"] = attribute(tb, sites_b, c[0])
            e["clusters"].append(item)
        e["n_clusters"] = len(clusters)
        segs.append(e)
    loc["bytes"] = segs
    if rng:
        ra, rb = rdir / f"peer-{pa}" / "rng-digest.bin", rdir / f"peer-{pb}" / "rng-digest.bin"
        if ra.is_file() and rb.is_file():
            pa_d, pb_d = RB.parse_digest(ra), RB.parse_digest(rb)
            dv = RB.digest_divergence(pa_d, pb_d)
            loc["rng"] = {"digest_divergence": dv, "first_call": None, "phases_compared": min(len(pa_d), len(pb_d))}
            if dv is None:
                loc["notes"].append(f"RNG streams identical on both peers in every phase up to turn {t}: the divergence is not an RNG call difference")
            else:
                la, lb = rdir / f"peer-{pa}" / "rng.log.bin", rdir / f"peer-{pb}" / "rng.log.bin"
                if dv["turn"] == t - 1 and la.is_file() and lb.is_file():
                    loc["rng"]["window"] = f"{t - 1}-{t - 1}"
                    loc["rng"]["first_call"] = RB.log_divergence(la, lb)
                else:
                    loc["notes"].append(f"first differing RNG phase is {dv['turn']} (log window was {t - 1}); rerun with --rng-log {dv['turn']}-{dv['turn']} for the call")
        else:
            loc["notes"].append("no rng-digest.bin (DLL without HeadlessDiagInstall?)")
    return loc


def render_md(cfg: dict, res: dict, loc: dict | None) -> str:
    md = [f"# Lockstep divergence: {cfg['name']}", ""]
    md.append(f"- status: **{res['status']}**, frames {res['frames']}, peers {[(p['player']) for p in cfg['peers']]} (peer i = active player), "
              f"ai_sends {cfg['ai_sends']}, turn_barrier {cfg['turn_barrier']}, order {cfg['order']}, remote_delay {cfg.get('remote_delay', 0)}")
    for d in res.get("divergences", [])[:3]:
        md.append(f"- turn **{d['turn']}**: " + " vs ".join(f"peers {g['peers']} (players {g['players']}) `{g['digest']}`" for g in d["groups"]))
    if not loc:
        return "\n".join(md) + "\n"
    md += ["", f"## Segments at turn {loc['turn']} (peer {loc['peer_a']} vs peer {loc['peer_b']})", "", ", ".join(loc["segments"]) or "-", "",
           f"Rerun with state dump: {loc['rerun']['status']}, reproduced: **{loc['reproduced']}** ({loc['rerun']['dll']})", ""]
    for s in loc.get("bytes", []):
        if s.get("error"):
            md.append(f"### {s['name']}\n\n{s['error']}\n")
            continue
        md += [f"### {s['name']}", "", f"sizes {s['size_a']} / {s['size_b']}, first differing byte {s['first_diff_offset']}, {s['n_diff_bytes']} differing byte(s), "
               f"{s['n_clusters']} cluster(s)", ""]
        for c in s["clusters"]:
            wa = c.get("writer_a") or {}
            site = wa.get("site") or {}
            md.append(f"- [{c['start']}, {c['end']}) a `{c['a']}` b `{c['b']}`" + (f" written by `{site.get('function')}` {Path(site.get('file', '')).name}:{site.get('line')}" if site else ""))
            if wa.get("stack"):
                md.append(f"  - stack: {' <- '.join(wa['stack'])}")
        md.append("")
    if loc.get("rng"):
        md += DY._render_rng({"rng": loc["rng"]})
    for n in loc.get("notes", []):
        md.append(f"- note: {n}")
    return "\n".join(md) + "\n"
