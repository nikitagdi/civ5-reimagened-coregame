"""civsim.saveio.report — masking of personal data (SteamIDs, nicknames) and rendering of inspect output.

Reports, index.json and envelope.json NEVER carry nicknames or SteamIDs. Raw bytes live only
in headless/out (pregame.bin etc.), which is git-ignored.
"""
from __future__ import annotations

import re
from typing import Any

from .binio import Span

_LEKMOD_TAG = re.compile(r"(?i)(lekmod v\d+(?:\.\d+)*)\s*$")
_STEAMID = re.compile(r"(?<!\d)7656119\d{10}(?!\d)")
_NICK_WITH_ID = re.compile(r"\^[^\^\r\n]{0,64}\^@?(?:\d{15,20})?")
_WIN_USER_PATH = re.compile(r"(?i)[a-z]:\\users\\[^\\/\s]+")

# fields whose value is a person's own text: never printed, only their presence/count
SENSITIVE_STR = {
    "archive.admin_password", "archive.alias", "archive.local_player_email_address", "archive.smtp_host",
    "archive.load_file_name",
}
SENSITIVE_VEC = {
    "slot_hints.nicknames", "archive.nicknames", "archive.leader_names", "archive.civ_passwords",
    "archive.email_addresses", "archive.turn_notify_email_address",
}
# vectors that are printed element-wise but through mask_text (type keys, TXT_KEYs, ...)
BULK_VEC = {"slot_hints.civilizations", "slot_hints.civilization_keys", "slot_hints.leader_keys", "archive.minor_civ_types",
            "archive.player_colors", "archive.civ_adjectives", "archive.civ_descriptions", "archive.civ_short_descriptions",
            "archive.flag_decals"}


def mask_text(s: str) -> str:
    """Replace SteamID64-looking digit runs, `^Nick^@<id>` forms and Windows user-profile paths."""
    if not s:
        return s
    s = _NICK_WITH_ID.sub("player#", s)
    s = _STEAMID.sub("steamid#", s)
    s = _WIN_USER_PATH.sub(r"<user>", s)
    return s


def public_fields(fields: dict[str, Any]) -> dict[str, Any]:
    """Fields safe for JSON reports: sensitive strings/vectors reduced to presence, everything else through mask_text."""
    out: dict[str, Any] = {}
    for k, v in fields.items():
        if k == "archive.game_name":                    # host-typed text + version tag: keep the tag, drop anything typed
            m = _LEKMOD_TAG.search(v)
            out[k] = m.group(1) if m else {"masked": True, "empty": not v}
        elif k in SENSITIVE_STR:
            out[k] = {"masked": True, "empty": not v}
        elif k in SENSITIVE_VEC:
            out[k] = {"masked": True, "count": len(v), "nonempty": sum(1 for x in v if x)}
        elif isinstance(v, str):
            out[k] = mask_text(v)
        elif isinstance(v, list) and v and isinstance(v[0], str):
            out[k] = [mask_text(x) for x in v]
        elif isinstance(v, list) and v and isinstance(v[0], tuple):
            out[k] = [[mask_text(a), b] for a, b in v]
        elif isinstance(v, dict):
            out[k] = {kk: (mask_text(vv) if isinstance(vv, str) else vv) for kk, vv in v.items()}
        else:
            out[k] = v
    return out


def group_spans(spans: list[Span]) -> list[dict[str, Any]]:
    """Collapse leaf spans into the top-level groups shown by `inspect --segments`."""
    groups: list[dict[str, Any]] = []

    def gname(sp: Span) -> str:
        n = sp.name
        if n.startswith("hdr.dlc"):
            return "exe.header.dlc"
        if n.startswith("hdr.mod"):
            return "exe.header.mods"
        if n.startswith("hdr.preview"):
            return "exe.header.preview"
        if n.startswith("hdr.tail"):
            return "exe.header.tail"
        if n.startswith("hdr."):
            return "exe.header.main"
        if n.startswith("zstream.chunk["):
            return "exe.zstream.chunks"
        if n.startswith("slot_hints."):
            return "dll.pregame.slot_hints"
        if n.startswith("archive."):
            return "dll.pregame.archive"
        if n.startswith("unparsed."):
            return n
        return n
    for sp in sorted(spans, key=lambda x: x.off):
        g = gname(sp)
        if groups and groups[-1]["name"] == g and groups[-1]["end"] == sp.off:
            groups[-1]["end"] = sp.end
            groups[-1]["fields"] += 1
        else:
            groups.append({"name": g, "offset": sp.off, "end": sp.end, "fields": 1, "kind": sp.kind, "owner": sp.owner})
    for g in groups:
        g["length"] = g["end"] - g["offset"]
    return groups


def render_text(summary: dict[str, Any], groups: list[dict[str, Any]] | None, extra: list[str] | None = None) -> str:
    L = []
    L.append(f"status      : {summary['status']}" + (f"  ({summary['reason']})" if summary.get("reason") else ""))
    L.append(f"sha256      : {summary['sha256']}")
    L.append(f"size        : {summary['size']}")
    L.append(f"game        : {summary.get('game_version')} build {summary.get('build_number')}  format v{summary.get('format_version')}")
    L.append(f"turn        : {summary.get('turn')}   civ {summary.get('civ')}   {summary.get('difficulty')}   {summary.get('game_speed')}   "
             f"{summary.get('world_size')}   {summary.get('map_script')}")
    L.append(f"lekmod tag  : {summary.get('lekmod_tag')}")
    if "players" in summary:
        L.append(f"slots       : {summary['players']} active ({summary.get('humans')} human)   game_started={summary.get('game_started')}")
    L.append(f"dlc / mods  : {summary.get('dlc_count')} / {summary.get('mod_count')}")
    if "body_size" in summary:
        L.append(f"body        : {summary['body_size']} bytes (sync-flush end: {summary.get('ends_with_sync_flush')}, "
                 f"final block seen: {summary.get('body_complete_stream')})")
    for w in summary.get("warnings", []):
        L.append(f"warning     : {w}")
    if groups:
        L.append("")
        L.append("segments:")
        for g in groups:
            L.append(f"  0x{g['offset']:08x} +{g['length']:<9d} {g['kind']:<6s} {g['owner']:<5s} {g['name']}  ({g['fields']} fields)")
    if extra:
        L.extend(extra)
    return "\n".join(L)
