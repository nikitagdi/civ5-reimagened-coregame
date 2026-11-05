"""Content of observation.json, events.json and errors.json (V1: skeleton, no codecs).

observation 'pending' is generated from the ENDTURN_BLOCKING_* enum and from the code that sets each type
(CvNotifications::GetEndTurnBlockedType, CvGame::updateTestEndTurn); a type without a row in PENDING_ARGS fails the build.
"""
from __future__ import annotations

import re
from pathlib import Path

from . import dllparse

API = "1.0"

# (name, type, binary type, source in the DLL, visible to role 'player' when)
_GAME = [
    ("turn", "int", "u16", "CvGame::getGameTurn()", "always"),
    ("era", "enum:Eras", "u8", "CvTeam::GetCurrentEra() of the player's team", "always"),
    ("speed", "enum:GameSpeeds", "u8", "CvGame::getGameSpeedType()", "always"),
    ("options", "map:string->bool", "bitset", "CvGame::isOption(name) for every GameOption", "always"),
    ("victories", "list:enum:Victories", "u8[]", "CvGame::isVictoryValid(victory)", "always"),
    ("turn_mode", "enum:TurnModes", "u8", "pregame turn mode (simultaneous / dynamic / sequential)", "always"),
    ("timer", "obj{elapsed,max,paused}", "f32,f32,u8", "CvGame::getTimeElapsed(), getMaxTurnLen(), m_bIsPaused", "always"),
    ("state_hash", "string(hex)", "u8[32]", "determinism-lab 'turn' level digest", "always"),
]
_ME = [
    ("gold", "int", "i32", "CvPlayer::GetTreasury()->GetGold()", "own"),
    ("gold_per_turn", "int", "i32", "CvPlayer::calculateGoldRate()", "own"),
    ("science_per_turn", "int", "i32", "CvPlayer::GetScience()", "own"),
    ("culture", "int", "i32", "CvPlayer::getJONSCulture()", "own"),
    ("culture_per_turn", "int", "i32", "CvPlayer::GetTotalJONSCulturePerTurn()", "own"),
    ("faith", "int", "i32", "CvPlayer::GetFaith()", "own"),
    ("faith_per_turn", "int", "i32", "CvPlayer::GetTotalFaithPerTurn()", "own"),
    ("happiness", "int", "i32", "CvPlayer::GetExcessHappiness()", "own"),
    ("golden_age_turns", "int", "i16", "CvPlayer::getGoldenAgeTurns()", "own"),
    ("policies", "list:enum:Policies", "u16[]", "CvPlayerPolicies::HasPolicy(policy)", "own"),
    ("branches", "list:enum:PolicyBranches", "u8[]", "CvPlayerPolicies::IsPolicyBranchUnlocked(branch)", "own"),
    ("next_policy_cost", "int", "i32", "CvPlayer::getNextPolicyCost()", "own"),
    ("free_policies", "int", "i16", "CvPlayer::GetNumFreePolicies()", "own"),
    ("techs", "list:enum:Technologies", "u16[]", "CvTeamTechs::HasTech(tech) of the player's team", "own"),
    ("research_queue", "list:enum:Technologies", "u16[]", "CvPlayerTechs (current research and queue)", "own"),
    ("free_techs", "int", "i16", "CvPlayer::GetNumFreeTechs()", "own"),
    ("free_great_people", "int", "i16", "CvPlayer::GetNumFreeGreatPeople()", "own"),
    ("faith_great_people", "int", "i16", "CvPlayer::GetNumFaithGreatPeople()", "own"),
    ("maya_boosts", "int", "i16", "CvPlayer::GetNumMayaBoosts()", "own"),
    ("religion", "obj", "struct", "CvPlayerReligions::GetReligionCreatedByPlayer(), pantheon, beliefs", "own"),
    ("ideology", "enum:PolicyBranches|null", "i8", "CvPlayerPolicies::GetLateGamePolicyTree()", "own"),
    ("spies", "list:obj", "struct[]", "CvPlayer::GetEspionage() spy list", "own"),
    ("trade_routes", "list:obj", "struct[]", "CvPlayer::GetTrade()", "own"),
    ("score", "int", "i32", "CvPlayer::GetScore()", "own"),
]
_PLAYERS = [
    ("id", "player", "u8", "CvPlayer::GetID()", "met"),
    ("team", "team", "u8", "CvPlayer::getTeam()", "met"),
    ("civ", "enum:Civilizations", "u8", "CvPlayer::getCivilizationType()", "met"),
    ("alive", "bool", "u8", "CvPlayer::isAlive()", "met"),
    ("human", "bool", "u8", "CvPlayer::isHuman()", "met"),
    ("at_war", "bool", "u8", "CvTeam::isAtWar(team of the viewer)", "met"),
    ("score", "int", "i32", "CvPlayer::GetScore() (the public score only)", "met"),
    ("diplomacy", "obj", "struct", "CvDiplomacyAI approach/attitude as shown on the diplomacy screen", "met"),
]
_PLOTS = [
    ("x,y", "plot", "u8,u8", "CvPlot::getX(), getY()", "revealed"),
    ("terrain", "enum:Terrains", "u8", "CvPlot::getTerrainType()", "revealed"),
    ("feature", "enum:Features", "i8", "CvPlot::getFeatureType()", "revealed"),
    ("resource", "enum:Resources|null", "i8", "CvPlot::getResourceType(viewer team) (tech-gated)", "revealed"),
    ("improvement", "enum:Improvements|null", "i8", "CvPlot::getRevealedImprovementType(viewer team)", "revealed"),
    ("route", "enum:Routes|null", "i8", "CvPlot::getRevealedRouteType(viewer team)", "revealed"),
    ("owner", "player|null", "i8", "CvPlot::getRevealedOwner(viewer team)", "revealed"),
    ("visible", "bool", "u8", "CvPlot::isVisible(viewer team)", "revealed"),
]
_UNITS = [
    ("id,owner", "unit_ref", "u16,u8", "CvUnit::GetID(), getOwner()", "own | visible"),
    ("type", "enum:Units", "u16", "CvUnit::getUnitType()", "own | visible"),
    ("x,y", "plot", "u8,u8", "CvUnit::getX(), getY()", "own | visible"),
    ("hp", "int", "u8", "100 - CvUnit::getDamage()", "own | visible"),
    ("moves", "int", "i16", "CvUnit::movesLeft()", "own"),
    ("promotions", "list:enum:Promotions", "u16[]", "CvUnit::isHasPromotion(promotion)", "own"),
    ("orders", "list:obj", "struct[]", "CvUnit mission queue (CvUnitMission)", "own"),
]
_CITIES = [
    ("id,owner", "city_ref", "u16,u8", "CvCity::GetID(), getOwner()", "own | revealed"),
    ("name,x,y", "obj", "struct", "CvCity::getName(), getX(), getY()", "own | revealed"),
    ("population", "int", "u8", "CvCity::getPopulation()", "own | revealed"),
    ("defense", "obj{hp,strength}", "u16,u16", "CvCity::getDamage(), GetStrengthValue()", "own | revealed"),
    ("queue", "list:obj", "struct[]", "CvCity order queue (getOrderQueueLength() entries)", "own"),
    ("buildings", "list:enum:Buildings", "u16[]", "CvCity::GetCityBuildings()", "own"),
    ("citizens", "obj", "struct", "CvCity::GetCityCitizens() focus and worked plots", "own"),
]
_NOTIF = [
    ("id", "int", "u32", "CvNotifications lookup index", "own"),
    ("type", "enum:NotificationTypes", "u16", "CvNotifications::GetNotificationType", "own"),
    ("summary,text", "string,string", "str,str", "CvNotifications::GetNotificationSummary/GetNotificationStr", "own"),
    ("x,y,data1,data2", "int,int,int,int", "i32x4", "notification payload", "own"),
    ("dismissed", "bool", "u8", "CvNotifications dismissed flag", "own"),
]
_DEALS = [
    ("deals", "list:obj", "struct[]", "CvGameDeals current/proposed deals that involve the viewer", "own"),
    ("ai_offers", "list:obj", "struct[]", "pending AI offers/demands to the viewer (OPEN: popup state, OQ 7/8)", "own"),
]
_LEAGUE = [
    ("league", "obj|null", "struct", "CvGameLeagues active league: members, proposals, votes, host", "member"),
]
_LEGAL = [
    ("unit", "list:enum:Missions", "bitset", "CvUnitMission::CanStartMission / CvUnit::canDoCommand per mission/command, on request", "own, on request"),
    ("city", "obj", "bitsets", "CvCity::canTrain/canConstruct/canCreate/canMaintain, IsCanPurchase, CanBuyPlot, on request", "own, on request"),
    ("player", "obj", "bitsets", "CvPlayerTechs::CanResearch, CvPlayer::canAdoptPolicy, CanUnlockPolicyBranch, on request", "own, on request"),
]

PARTS = [
    ("game", "player", "CvGame", _GAME, None),
    ("me", "player", "CvPlayer of the session player", _ME, None),
    ("players", "player", "CvPlayer/CvTeam of met players", _PLAYERS, "Only players the viewer's team has met (CvTeam::isHasMet)."),
    ("plots", "player", "CvPlot revealed accessors", _PLOTS, "Only plots with isRevealed(viewer team); unrevealed plots are absent, not null."),
    ("units", "player", "CvPlayer::firstUnit/nextUnit", _UNITS, "Foreign units only on visible plots and not invisible to the viewer's team (CvUnit::isInvisible)."),
    ("cities", "player", "CvPlayer::firstCity/nextCity", _CITIES, "Foreign cities on revealed plots: banner-level information only."),
    ("pending", "player", "see 'pending' table", [("blocking", "list:obj{type,args}", "struct[]", "table 'pending' of this document", "own")], None),
    ("notifications", "player", "CvNotifications of the session player", _NOTIF, None),
    ("deals", "player", "CvGameDeals", _DEALS, None),
    ("league", "player", "CvGameLeagues", _LEAGUE, None),
    ("legal", "player", "can* functions", _LEGAL, "Expensive: only for the objects named in observe.legal_for."),
]

# type -> (what the client needs to answer, open question or None)
PENDING_ARGS: dict[str, tuple[str, str | None]] = {
    "ENDTURN_BLOCKING_POLICY": ("policies adoptable now (CvPlayer::canAdoptPolicy) and unlockable branches", None),
    "ENDTURN_BLOCKING_RESEARCH": ("researchable techs (CvPlayerTechs::CanResearch)", None),
    "ENDTURN_BLOCKING_PRODUCTION": ("city id from the notification and its legal orders (canTrain/canConstruct/canCreate/canMaintain)", None),
    "ENDTURN_BLOCKING_UNITS": ("ready units (CvPlayer::GetFirstReadyUnit, hasReadyUnit)", None),
    "ENDTURN_BLOCKING_DIPLO_VOTE": ("candidate players for diplo.vote", None),
    "ENDTURN_BLOCKING_MINOR_QUEST": ("-", "never assigned in v34.15: no source, cannot block"),
    "ENDTURN_BLOCKING_FREE_TECH": ("free tech count (GetNumFreeTechs) and techs with CanResearchForFree", None),
    "ENDTURN_BLOCKING_STACKED_UNITS": ("the ready unit that cannot hold its plot (CvUnit::canHold)", None),
    "ENDTURN_BLOCKING_UNIT_NEEDS_ORDERS": ("-", "never assigned in v34.15: no source, cannot block"),
    "ENDTURN_BLOCKING_UNIT_PROMOTION": ("promotable units and their legal promotions (CvUnit::canPromote)", "also depends on GAMEOPTION_PROMOTION_SAVING (a game option, readable)"),
    "ENDTURN_BLOCKING_CITY_RANGE_ATTACK": ("the city that can shoot (notification data)", "blocks only if the UI auto-end-turn preference is on (IsMPAutoEndTurnEnabled/IsSPAutoEndTurnEnabled, UI state of the active player): make it a session option, do not read the UI"),
    "ENDTURN_BLOCKING_FREE_POLICY": ("free policy count (GetNumFreePolicies) and adoptable policies", None),
    "ENDTURN_BLOCKING_FREE_ITEMS": ("free great person count (GetNumFreeGreatPeople) and choosable great people", None),
    "ENDTURN_BLOCKING_FOUND_PANTHEON": ("available pantheon beliefs (CvGameReligions::IsPantheonBeliefAvailable)", None),
    "ENDTURN_BLOCKING_FOUND_RELIGION": ("holy city from the notification (x, y), free religions and beliefs", None),
    "ENDTURN_BLOCKING_ENHANCE_RELIGION": ("enhance-able religion and available beliefs", None),
    "ENDTURN_BLOCKING_STEAL_TECH": ("steal-able techs per target (CvPlayerEspionage stealable list)", None),
    "ENDTURN_BLOCKING_MAYA_LONG_COUNT": ("Maya boost count (GetNumMayaBoosts) and choosable great people", None),
    "ENDTURN_BLOCKING_FAITH_GREAT_PERSON": ("faith great person count (GetNumFaithGreatPeople) and choosable great people", None),
    "ENDTURN_BLOCKING_ADD_REFORMATION_BELIEF": ("available reformation beliefs (CvGameReligions::CanAddReformationBelief)", None),
    "ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS": ("proposable resolutions (CvLeague::CanProposeEnact/CanProposeRepeal)", None),
    "ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY": ("archaeology choices of the pending dig", None),
    "ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES": ("votable resolutions and the player's votes (CvLeague::CanVote)", None),
    "ENDTURN_BLOCKING_CHOOSE_IDEOLOGY": ("selectable ideology branches (CanUnlockPolicyBranch)", None),
}
_UNIT_CHECK_SRC = {
    "ENDTURN_BLOCKING_UNIT_PROMOTION": "CvPlayer::hasPromotableUnit() in CvGame::updateTestEndTurn (CvGame.cpp:{line})",
    "ENDTURN_BLOCKING_STACKED_UNITS": "CvPlayer::hasReadyUnit() and !GetFirstReadyUnit()->canHold(plot) in CvGame::updateTestEndTurn (CvGame.cpp:{line})",
    "ENDTURN_BLOCKING_UNITS": "CvPlayer::hasReadyUnit() and canHold(plot) in CvGame::updateTestEndTurn (CvGame.cpp:{line})",
}


def blocking_types(dll_src: Path) -> list[str]:
    s = dllparse.blank_comments(dllparse.read(dll_src / "CvGameCoreDLLUtil/include/CvEnums.h"))
    body = re.search(r"enum EndTurnBlockingTypes\s*\{(.*?)\}", s, flags=re.S).group(1)
    return [t for t in re.findall(r"\b(ENDTURN_BLOCKING_[A-Z_]+)\b", body)]


def notification_map(dll_src: Path) -> dict[str, str]:
    """ENDTURN_BLOCKING_X -> NOTIFICATION_Y from CvNotifications::GetEndTurnBlockedType."""
    s = dllparse.blank_comments(dllparse.read(dll_src / "CvNotifications.cpp"))
    i = s.index("bool CvNotifications::GetEndTurnBlockedType")
    j = s.index("int CvNotifications::GetNumNotifications")
    out, cur = {}, None
    for m in re.finditer(r"case (NOTIFICATION_\w+):|eBlockingType = (ENDTURN_BLOCKING_\w+);", s[i:j]):
        if m.group(1):
            cur = m.group(1)
        elif cur:
            out[m.group(2)] = cur
    return out


def unit_check_types(dll_src: Path) -> dict[str, int]:
    """ENDTURN_BLOCKING_X -> line in CvGame.cpp where updateTestEndTurn assigns it."""
    s = dllparse.blank_comments(dllparse.read(dll_src / "CvGame.cpp"))
    return {m.group(1): s.count(chr(10), 0, m.start()) + 1 for m in re.finditer(r"eEndTurnBlockingType = (ENDTURN_BLOCKING_\w+);", s)}


def notification_line(dll_src: Path) -> int:
    s = dllparse.read(dll_src / "CvNotifications.cpp")
    return s.count(chr(10), 0, s.index("bool CvNotifications::GetEndTurnBlockedType")) + 1


def observation(dll_src: Path) -> dict:
    types = blocking_types(dll_src)
    notif, unit, nline = notification_map(dll_src), unit_check_types(dll_src), notification_line(dll_src)
    pending = []
    for t in types:
        if t not in PENDING_ARGS:
            raise ValueError(f"new end-turn blocking type {t}: add a row to content.PENDING_ARGS")
        args, open_q = PENDING_ARGS[t]
        if t in notif:
            kind, src = "notification", f"{notif[t]} in CvNotifications::GetEndTurnBlockedType (CvNotifications.cpp:{nline})"
            non_active = "computed from the player's own CvNotifications list: no active-player or UI dependency"
        elif t in unit:
            kind, src = "unit_check", _UNIT_CHECK_SRC[t].format(line=unit[t])
            non_active = "CvGame::updateTestEndTurn runs only for the active player, but its inputs are const CvPlayer/CvUnit queries valid for any player"
        else:
            kind, src, non_active = "unused", "no code assigns this type in v34.15", "-"
        pending.append({"type": t, "kind": kind, "source": src, "non_active": non_active, "args": args, "open": open_q})
    parts = [{"name": n, "view": v, "source": s_, "fields": [dict(zip(("name", "type", "bin", "source", "visible"), f)) for f in fl],
              **({"note": note} if note else {})} for n, v, s_, fl, note in PARTS]
    return {"schema": 1, "api": API,
            "views": {"player": "what the session player would see in the real game (fog of war applied on the server); default for role 'player'",
                      "full": "the whole authoritative state; roles observer/admin only, never role 'player'"},
            "delta": {"cursor": "frame number of the last state sent to this client",
                      "upsert": "objects by stable id (units, cities, notifications, deals) and changed plots",
                      "remove": "ids of vanished objects",
                      "snapshot": "sent on connect, on request and when the cursor is lost; every response carries state_hash"},
            "parts": parts, "pending": pending}


EVENT_SOURCES = {
    "notifications": "CvNotifications of the receiving player (already personal, survive save/load)",
    "diff": "difference of two frames by stable ids, filtered on the server by the receiver's visibility",
    "game_event": "Lua GameEvents hooks of the DLL (read-only listener)",
    "gameplay_tap": "ICvCombatInfo1 / Gameplay* sink through an optional read-only tap (never the active-player filtered path)",
    "session": "API session layer",
}
_EVENTS = [
    ("notification", "notifications", "player", ["notification"], "own"),
    ("turn.begin", "session", "player", ["turn", "pending_players", "timer"], "own"),
    ("turn.end", "session", "player", ["turn"], "own"),
    ("unit.created", "diff", "unit_ref", ["type", "plot"], "own | visible"),
    ("unit.moved", "diff", "unit_ref", ["from", "to"], "own | visible at either end"),
    ("unit.destroyed", "diff", "unit_ref", ["cause"], "own | visible"),
    ("unit.damaged", "diff", "unit_ref", ["hp"], "own | visible"),
    ("unit.promoted", "game_event", "unit_ref", ["promotion"], "own | visible"),
    ("city.founded", "diff", "city_ref", ["plot"], "own | revealed"),
    ("city.captured", "diff", "city_ref", ["old_owner", "new_owner"], "own | revealed"),
    ("city.razed", "diff", "city_ref", ["plot"], "own | revealed"),
    ("city.pop_changed", "diff", "city_ref", ["population"], "own | revealed"),
    ("plot.revealed", "diff", "plot", ["terrain", "owner"], "own"),
    ("plot.improved", "diff", "plot", ["improvement"], "own | revealed"),
    ("war.changed", "diff", "team", ["with", "war"], "met teams"),
    ("tech.acquired", "diff", "player", ["tech"], "own"),
    ("policy.adopted", "game_event", "player", ["policy"], "own"),
    ("combat", "gameplay_tap", "unit_ref", ["attacker", "defender", "damage", "interception"], "either participant visible"),
    ("cmd.rejected", "session", "command", ["cmd_id", "code", "detail"], "own"),
]


def events() -> dict:
    return {"schema": 1, "api": API,
            "envelope": {"seq": "per-client event counter", "turn": "game turn", "frame": "server frame",
                         "kind": "event kind", "subject": "object the event is about",
                         "data": "kind specific fields", "cause_cmd": "seq of the command that caused it (optional)"},
            "sources": EVENT_SOURCES,
            "events": [{"kind": k, "source": s, "subject": sub, "data": d, "visible_if": v} for k, s, sub, d, v in _EVENTS]}


_ERRORS = [
    ("parse_error", "range", True, False, "frame larger than the limit or not decodable"),
    ("unknown_command", "range", False, False, "name not in the dictionary"),
    ("bad_args", "range", False, False, "missing/unknown argument or wrong type"),
    ("out_of_range", "range", False, False, "numeric/plot argument outside its range"),
    ("string_invalid", "range", False, False, "control character or string longer than max_len"),
    ("reserved_value", "range", False, False, "raw multiplexer value (GiftUnit < 0, EnhanceReligion -1, MinorGiftGold > 100000); use the dedicated command"),
    ("forbidden", "class", False, False, "class server / pregame / unsupported"),
    ("role_forbidden", "class", False, False, "role does not allow the command or view"),
    ("player_mismatch", "player", False, False, "player argument must be the session player"),
    ("invalid_player", "player", False, False, "player/team id out of range, dead or of the wrong kind"),
    ("not_found", "ownership", False, False, "unit, city, plot or league does not exist"),
    ("not_owner", "ownership", False, False, "unit or city does not belong to the session player"),
    ("wrong_phase", "phase", False, False, "turn not active, game paused, loading or resyncing"),
    ("no_pending", "pending", False, False, "the decision is not pending for the session player"),
    ("pending_decision", "pending", False, False, "turn.complete while an end-turn blocker is pending"),
    ("not_applicable", "applicability", False, False, "the DLL can* function says no; detail names it"),
    ("rate_limited", "rate", False, False, "budget of the rate class exhausted"),
    ("api_version", "hello", True, False, "major version mismatch"),
    ("data_mismatch", "hello", False, False, "data_hash differs (enum ids may not match)"),
    ("not_implemented", "session", False, False, "V1 host reserves api_* commands and answers this"),
    ("internal", "session", True, False, "server fault; never caused by a command that was accepted"),
]


def errors() -> dict:
    return {"schema": 1, "api": API,
            "format": {"ack": "{cmd_id, status: 'rejected', code, detail}",
                       "error": "{ok: false, error: code, where: step, detail}  (same shape as the engine-host control channel)",
                       "rule": "a rejected command never reaches the DLL and is not written to commands.bin (only to the rejection log)"},
            "codes": [{"code": c, "step": s, "fatal": f, "reaches_dll": r, "meaning": m} for c, s, f, r, m in _ERRORS]}
