"""civsim.saveio.pregame — parser for the DLL-owned pregame block (CvPreGame::write = saveSlotHints v3 + writeArchive v6).

Layout source: LEKMOD_DLL/CvGameCoreDLL_Expansion2/CvPreGame.cpp (saveSlotHints ~464, writeArchive ~3348), CvInfos.cpp
(CvBaseInfo/World/Climate/SeaLevel/TurnTimer writeTo), FStlContainerSerialization.h (u32 count + elements; 32-bit size_t),
CvInfosSerializationHelper.h (WriteTypeArray = u32 count + string type ids). Each top-level field becomes one Span so
`inspect --segments` can show where everything lives; the block's end is validated against the position of the 8-byte
exe block + zlib header by envelope.py (a mismatch means "unsupported layout", never a crash).

Only v3 slot hints + v6 archive (the v34.15 writer) are parsed; other versions raise SaveFormatError.
"""
from __future__ import annotations

from typing import Any

from .binio import Reader, SaveFormatError, Span

SLOT_STATUS = {0: "OPEN", 1: "COMPUTER", 2: "CLOSED", 3: "TAKEN", 4: "OBSERVER"}
SLOT_CLAIM = {0: "UNASSIGNED", 1: "RESERVED", 2: "ASSIGNED"}
GAME_TYPE = {-1: "NONE", 0: "SINGLE_PLAYER", 1: "NETWORK_MULTIPLAYER", 2: "HOTSEAT_MULTIPLAYER", 3: "EMAIL_MULTIPLAYER"}
SLOT_HINTS_VERSION = 3
ARCHIVE_VERSION = 6
MAX_PLAYERS = 64


def _info_base(r: Reader) -> dict[str, Any]:
    """CvBaseInfo::writeTo: i32 id + 8 strings (civilopedia, description, help, disabledHelp, strategy, type, textKey, text)."""
    d: dict[str, Any] = {"id": r.i32()}
    for k in ("civilopedia", "description", "help", "disabled_help", "strategy", "type", "text_key", "text"):
        d[k] = r.string()
    return d


def _group(r: Reader, name: str, fn) -> Any:
    """Run `fn` (unnamed reads) and record a single span for everything it consumed."""
    s = r.pos
    v = fn()
    r.spans.append(Span(name, s, r.pos - s, r.kind, r.owner))
    r.fields[name] = v
    return v


def _info_world(r: Reader) -> dict[str, Any]:
    ver = r.i32()
    if ver != 2:
        raise SaveFormatError(f"CvWorldInfo version {ver} (only 2 supported)", r.pos - 4)
    d = _info_base(r)
    d["version"] = ver
    for k in ("default_players", "default_minor_civs", "fog_tiles_per_barb_camp", "num_natural_wonders", "unit_name_modifier",
              "target_num_cities", "num_free_building_resources", "building_class_prereq_modifier", "max_conscript_modifier",
              "grid_width", "grid_height", "max_active_religions", "terrain_grain_change", "feature_grain_change",
              "research_percent", "advanced_start_points_mod", "num_cities_unhappiness_percent",
              "num_cities_policy_cost_mod", "num_cities_tech_cost_mod"):
        d[k] = r.i32()
    return d


def _info_climate(r: Reader) -> dict[str, Any]:
    d = _info_base(r)
    for k in ("desert_percent_change", "jungle_latitude", "hill_range", "mountain_percent"):
        d[k] = r.i32()
    for k in ("snow_lat", "tundra_lat", "grass_lat", "desert_bottom_lat", "desert_top_lat", "ice_lat", "rand_ice_lat"):
        d[k] = r.u32()      # float bit patterns
    return d


def _info_sealevel(r: Reader) -> dict[str, Any]:
    d = _info_base(r)
    d["sea_level_change"] = r.i32()
    return d


def _info_turntimer(r: Reader) -> dict[str, Any]:
    d = _info_base(r)
    for k in ("base_time", "city_resource", "unit_resource", "first_turn_multiplier"):
        d[k] = r.i32()
    return d


def _custom_options(r: Reader, name: str) -> list[tuple[str, int]]:
    """std::vector<CustomOption>: u32 count, each {string name, i32 value}."""
    s = r.pos
    n = r._count(name, 8)
    out = []
    for _ in range(n):
        nm = r.string()
        out.append((nm, r.i32()))
    r.spans.append(Span(name, s, r.pos - s, r.kind, r.owner))
    r.fields[name] = out
    return out


def _type_array(r: Reader, name: str) -> list[str]:
    """CvInfosSerializationHelper::WriteTypeArray: u32 count + one string type id per entry (empty = missing)."""
    return r.vec_str(name)


def parse_pregame(data: bytes, start: int, end: int | None = None) -> tuple[list[Span], dict[str, Any], int]:
    """Parse CvPreGame::write output at data[start:]. Returns (spans, fields, end_offset).

    `end` bounds the reader (the caller usually passes len(data)); the true block end is the returned offset.
    """
    r = Reader(data, start, end, kind="dll", owner="dll")
    f = r.fields
    # ---- saveSlotHints ------------------------------------------------------------------
    ver = r.u32("slot_hints.version")
    if ver != SLOT_HINTS_VERSION:
        raise SaveFormatError(f"saveSlotHints version {ver} (only {SLOT_HINTS_VERSION} supported)", start)
    r.i32("slot_hints.game_speed")
    r.i32("slot_hints.world_size")
    r.string("slot_hints.map_script")
    r.vec_i32("slot_hints.civilizations")
    r.vec_str("slot_hints.nicknames")
    r.vec_i32("slot_hints.slot_status")
    r.vec_i32("slot_hints.slot_claims")
    r.vec_i32("slot_hints.team_types")
    r.vec_i32("slot_hints.handicaps")
    r.vec_str("slot_hints.civilization_keys")
    r.vec_str("slot_hints.leader_keys")
    for k in ("civilizations", "nicknames", "slot_status", "slot_claims", "team_types", "handicaps", "civilization_keys", "leader_keys"):
        if len(f[f"slot_hints.{k}"]) != MAX_PLAYERS:
            raise SaveFormatError(f"slot_hints.{k}: {len(f[f'slot_hints.{k}'])} entries, expected {MAX_PLAYERS}", r.pos)
    # ---- writeArchive v6 ------------------------------------------------------------------
    av = r.u32("archive.version")
    if av != ARCHIVE_VERSION:
        raise SaveFormatError(f"writeArchive version {av} (only {ARCHIVE_VERSION} supported)", r.pos - 4)
    A = "archive."
    r.i32(A + "active_player")
    r.string(A + "admin_password")
    r.i32(A + "advanced_start_points")
    r.string(A + "alias")
    r.vec_i32(A + "art_styles")
    r.boolean(A + "autorun")
    r.f32(A + "autorun_turn_delay")
    r.i32(A + "autorun_turn_limit")
    r.i32(A + "bandwidth")
    r.i32(A + "calendar")
    _group(r, A + "calendar_info", lambda: _info_base(r))
    r.vec_str(A + "civ_adjectives")
    r.vec_str(A + "civ_descriptions")
    r.vec_str(A + "civ_passwords")
    r.vec_str(A + "civ_short_descriptions")
    r.i32(A + "climate")
    _group(r, A + "climate_info", lambda: _info_climate(r))
    r.i32(A + "era")
    r.vec_str(A + "email_addresses")
    r.f32(A + "end_turn_timer_length")
    r.vec_str(A + "flag_decals")
    r.vec_bool(A + "deprecated_force_controls")
    r.i32(A + "game_mode")
    r.string(A + "game_name")
    r.i32(A + "game_speed")
    r.boolean(A + "game_started")
    r.i32(A + "game_turn")
    r.i32(A + "game_type")
    r.u8(A + "game_map_type")       # 1 byte in the saves although CvPreGame.cpp reads an enum: the 5 bytes before `handicaps` are
    r.i32(A + "game_update_time")   # always zero, so 1+4 vs 4+1 is not distinguishable from the corpus
    r.vec_i32(A + "handicaps")
    r.vec_i32(A + "last_human_handicaps")
    r.boolean(A + "is_earth_map")
    r.boolean(A + "is_internet_game")
    r.vec_str(A + "leader_names")
    r.string(A + "load_file_name")
    r.string(A + "local_player_email_address")
    r.boolean(A + "map_no_players")
    r.u32(A + "map_random_seed")
    r.boolean(A + "load_wb_scenario")
    r.boolean(A + "override_scenario_handicap")
    r.string(A + "map_script_name")
    r.i32(A + "max_city_elimination")
    r.i32(A + "max_turns")
    r.i32(A + "num_minor_civs")
    _type_array(r, A + "minor_civ_types")
    r.vec_bool(A + "minor_nation_civs")
    r.boolean(A + "dummy_value")
    r.vec_bool(A + "multiplayer_options")
    r.vec_i32(A + "net_ids")
    r.vec_str(A + "nicknames")
    r.i32(A + "num_victory_infos")
    r.i32(A + "pit_boss_turn_time")
    r.vec_bool(A + "playable_civs")
    _type_array(r, A + "player_colors")
    r.boolean(A + "private_game")
    r.boolean(A + "quick_combat")
    r.boolean(A + "quick_combat_default")
    r.i32(A + "quick_handicap")
    r.boolean(A + "quickstart")
    r.boolean(A + "random_world_size")
    r.boolean(A + "random_map_script")
    r.vec_bool(A + "ready_players")
    r.i32(A + "sea_level")
    _group(r, A + "sea_level_info", lambda: _info_sealevel(r))
    r.boolean(A + "dummy_value2")
    r.vec_i32(A + "slot_claims")
    r.vec_i32(A + "slot_status")
    r.string(A + "smtp_host")
    r.u32(A + "sync_random_seed")
    r.i32(A + "target_score")
    r.vec_i32(A + "team_types")
    r.boolean(A + "transferred_map")
    _group(r, A + "turn_timer", lambda: _info_turntimer(r))
    r.i32(A + "turn_timer_type")
    r.boolean(A + "city_screen_blocked")
    r.vec_bool(A + "victories")
    r.vec_bool(A + "white_flags")
    _group(r, A + "world_info", lambda: _info_world(r))
    r.i32(A + "world_size")
    _custom_options(r, A + "game_options")
    _custom_options(r, A + "map_options")
    r.string(A + "version_string")
    r.vec_bool(A + "turn_notify_steam_invite")
    r.vec_bool(A + "turn_notify_email")
    r.vec_str(A + "turn_notify_email_address")
    return r.spans, r.fields, r.pos


# ---------------------------------------------------------------------------------- interpretation
def slot_table(fields: dict[str, Any]) -> list[dict[str, Any]]:
    """Per-slot view from the slot hints (authoritative for readActiveSlotCountFromSaveGame). Nicknames are NOT included."""
    st = fields["slot_hints.slot_status"]
    cl = fields["slot_hints.slot_claims"]
    out = []
    for i in range(len(st)):
        out.append({
            "slot": i,
            "status": SLOT_STATUS.get(st[i], str(st[i])),
            "claim": SLOT_CLAIM.get(cl[i], str(cl[i])),
            "civ": fields["slot_hints.civilization_keys"][i],
            "leader": fields["slot_hints.leader_keys"][i],
            "team": fields["slot_hints.team_types"][i],
            "handicap": fields["slot_hints.handicaps"][i],
            "has_nickname": bool(fields["slot_hints.nicknames"][i]),
        })
    return out


def active_slot_count(fields: dict[str, Any]) -> int:
    """CvPreGame::calcActiveSlotCount: status in {TAKEN, COMPUTER, OBSERVER} and claim in {ASSIGNED, RESERVED}."""
    n = 0
    for s, c in zip(fields["slot_hints.slot_status"], fields["slot_hints.slot_claims"]):
        if s in (1, 3, 4) and c in (1, 2):
            n += 1
    return n
