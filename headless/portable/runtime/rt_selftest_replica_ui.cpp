// Scenario `replicaui` (B4 M2 WO-1; run.sh --scenario replicaui): a replica serving a Lua UI, through the C ABI only (plus the game objects to count what the
// engine should have announced).
//  The core is a process singleton, so one instance plays both parts, as tests/real_replica.rs does: as the server it plays `turns` turns of two test-bot humans and `ai` AI and keeps
//  the journal and the replica digest after every non-empty batch; then it becomes a replica of seat 0 from the first snapshot (gc_set_role, gc_resync) with a separate Lua state as its UI:
//   - gc_lua_attach_ui: Players[0]:GetName(), GameInfo.Units(), DB.Query("PRAGMA table_info(Units)"), methods that are not in this DLL are nil, the stack of the UI state is unchanged;
//   - gc_ui_setup_graphical announces exactly one "created" per living unit (an embarked one: embark) and city, all flagged REBUILD;
//   - the journal is applied batch by batch; between two batches a Lua script reads ~100 getters of Game, Player, City, Unit, Plot and the digest equals the server's after every batch
//     (the UI reading and the interface state never change the game); GameEvents.PlayerAdoptPolicy (subscribed) comes out of the sink when an AI adopts a policy (turns >= 30);
//   - gc_ui_op: select a unit (UNIT_SELECTION_CHANGED), interface mode, city selection, notifications; the digest does not move;
//   - gc_resync to the last snapshot, then gc_lua_invalidate_ui: a method of a unit object kept from before is a Lua error, Players[0]:GetName() still works, the digest is the server's.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "rt_active.h"
#include "rt_selftest.h"
#include "rt_selftest_slots.h"
#include "rt_session.h"

using namespace lekmod_portable;

namespace {

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

std::string Escape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		const char ch = s[i];
		if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
		else if ((unsigned char)ch >= 0x20) out += ch;
	}
	return out;
}

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}
	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[2048];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("replicaui: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct Seen
{
	const gc_host* outer;
	int logErrors, luaErrors;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), luaErrors(0) {}
};

void OnLog(void* user, int32_t level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == GC_LOG_ERROR) { ++s->logErrors; if (s->lines.size() < 20) s->lines.push_back(std::string("log error: ") + msg); }
	if (s->outer && s->outer->log) s->outer->log(s->outer->user, level, msg);
}

int32_t OnReadFile(void* user, const char* path, gc_buf* out)
{
	Seen* s = static_cast<Seen*>(user);
	return s->outer->read_file(s->outer->user, path, out);
}

void OnEvent(void* user, const gc_event* ev)
{
	Seen* s = static_cast<Seen*>(user);
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 20) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
}

// What the sink received.
struct Rec
{
	int kind;
	unsigned flags;
	std::vector<int> args;
	std::string text, text2;
	std::vector<uint8_t> data;
};

struct Sink
{
	std::vector<Rec> events;
	int counts[200];
	Sink() { memset(counts, 0, sizeof(counts)); }
	void Clear() { events.clear(); memset(counts, 0, sizeof(counts)); }
	int Count(int kind) const { return kind >= 0 && kind < 200 ? counts[kind] : 0; }
};

void OnUiEvent(void* user, const gc_ui_event* ev)
{
	Sink* s = static_cast<Sink*>(user);
	Rec r;
	r.kind = ev->kind;
	r.flags = ev->flags;
	r.args.assign(ev->args, ev->args + ev->n_args);
	if (ev->text) r.text = ev->text;
	if (ev->text2) r.text2 = ev->text2;
	if (ev->data && ev->data_len) r.data.assign(ev->data, ev->data + ev->data_len);
	if (ev->kind >= 0 && ev->kind < 200) ++s->counts[ev->kind];
	s->events.push_back(r);
}

uint64_t Digest(gc_instance* inst, int viewer)
{
	gc_error err;
	uint64_t d = 0;
	if (gc_state_digest(inst, viewer, &d, &err) != GC_OK) return 0;
	return d;
}

std::string Hex(uint64_t v) { return Format("%016llx", (unsigned long long)v); }

bool Phase(gc_instance* inst, int* phase, int* turn)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	if (gc_query(inst, GC_Q_GAME, NULL, 0, &b, &err) != GC_OK) return false;
	gcq_game g;
	const bool ok = b.len >= sizeof(gcq_header) + sizeof(gcq_game);
	if (ok)
	{
		memcpy(&g, b.data + sizeof(gcq_header), sizeof(g));
		*phase = (int)g.phase;
		*turn = (int)g.turn;
	}
	gc_buf_release(&b);
	return ok;
}

// Pumps until the game can be saved at a turn after `after`; every pump's batch goes to `batches` with the digest of the server right after it (0 when the batch is empty).
struct Batch
{
	std::vector<uint8_t> bytes;
	uint64_t digest;
};

bool PumpToSavePoint(gc_instance* inst, int after, std::vector<Batch>& batches, int* turn, std::vector<uint8_t>& snap, std::string& why)
{
	for (int i = 0; i < 20000; ++i)
	{
		gc_pump_result r;
		gc_error err;
		memset(&err, 0, sizeof(err));
		if (gc_pump(inst, 64, &r, &err) != GC_OK) { why = Format("gc_pump: %d %s", err.code, err.message); return false; }
		int phase = 0, t = 0;
		const bool humans = Phase(inst, &phase, &t) && phase == 0;
		gc_buf jb = {0, 0, 0, 0};
		if (gc_journal_take(inst, humans ? GC_JOURNAL_TAKE_CHECK : 0, &jb, &err) != GC_OK) { why = Format("gc_journal_take: %d %s", err.code, err.message); return false; }
		Batch b;
		b.bytes.assign(jb.data, jb.data + jb.len);
		gc_buf_release(&jb);
		// The digest after a batch that did something (records > 0): the replica must be there after applying it.
		b.digest = (b.bytes.size() >= 12 && (b.bytes[8] | b.bytes[9] | b.bytes[10] | b.bytes[11]) != 0) ? Digest(inst, -3) : 0;
		batches.push_back(b);
		if (r.turn > after)
		{
			gc_buf sb = {0, 0, 0, 0};
			if (gc_save(inst, &sb, &err) == GC_OK)
			{
				snap.assign(sb.data, sb.data + sb.len);
				gc_buf_release(&sb);
				*turn = r.turn;
				return true;
			}
		}
	}
	why = "no save point";
	return false;
}

int LuaErrorLine(lua_State* L, int rc, std::string& out)
{
	if (rc == 0) return 0;
	const char* m = lua_tostring(L, -1);
	out = m ? m : "?";
	lua_pop(L, 1);
	return rc;
}

// Runs a chunk in L; returns "" or the error message. The stack is as before.
std::string RunLua(lua_State* L, const char* code, const char* name)
{
	const int top = lua_gettop(L);
	std::string err;
	int rc = luaL_loadbuffer(L, code, strlen(code), name);
	if (rc == 0) rc = lua_pcall(L, 0, 0, 0);
	LuaErrorLine(L, rc, err);
	lua_settop(L, top);
	return err;
}

// Evaluates "return <expr>" as a string.
std::string EvalLua(lua_State* L, const std::string& expr, bool* ok = NULL)
{
	const int top = lua_gettop(L);
	std::string code = "return tostring(" + expr + ")";
	std::string out;
	int rc = luaL_loadbuffer(L, code.c_str(), code.size(), "=eval");
	if (rc == 0) rc = lua_pcall(L, 0, 1, 0);
	if (rc != 0)
	{
		LuaErrorLine(L, rc, out);
		if (ok) *ok = false;
	}
	else
	{
		const char* s = lua_tostring(L, -1);
		out = s ? s : "";
		if (ok) *ok = true;
	}
	lua_settop(L, top);
	return out;
}

// The getters of the UI between two batches: every call is protected so one missing method is counted, not fatal; the sum of the answers is returned so the optimizer
// of nobody removes the calls. "calls" and "errors" are globals of the UI state.
const char* const kGetterScript =
	"local calls, errors, missing = 0, 0, 0\n"
	"local function try(f, ...) if f == nil then missing = missing + 1 return nil end calls = calls + 1 local ok, v = pcall(f, ...) if not ok then errors = errors + 1 LastGetterError = tostring(v) return nil end return v end\n"
	"local me = Game.GetActivePlayer()\n"
	"try(Game.GetGameTurn) try(Game.GetActiveTeam) try(Game.GetMaxTurns) try(Game.IsNetworkMultiPlayer) try(Game.GetGameSpeedType) try(Game.GetStartTurn) try(Game.GetTurnString)\n"
	"try(Game.GetNumGameTurnActive) try(Game.IsGameMultiPlayer) try(Game.GetEstimateEndTurn) try(Game.CountHumanPlayersAlive) try(Game.GetNumCities) try(Game.GetWinner)\n"
	"for p = 0, GameDefines.MAX_MAJOR_CIVS - 1 do\n"
	"  local pl = Players[p]\n"
	"  if pl and try(pl.IsAlive, pl) then\n"
	"    try(pl.GetName, pl) try(pl.GetCivilizationType, pl) try(pl.GetTeam, pl) try(pl.IsHuman, pl) try(pl.GetNumCities, pl) try(pl.GetCurrentEra, pl) try(pl.GetGold, pl)\n"
	"    try(pl.CalculateGoldRate, pl) try(pl.GetTotalJONSCulturePerTurn, pl) try(pl.GetJONSCulture, pl) try(pl.GetNextPolicyCost, pl) try(pl.GetNumPolicies, pl) try(pl.IsPolicyBranchUnlocked, pl, 0)\n"
	"    try(pl.GetCurrentResearch, pl) try(pl.GetScience, pl) try(pl.GetHappiness, pl) try(pl.GetExcessHappiness, pl) try(pl.IsGoldenAge, pl) try(pl.GetGoldenAgeTurns, pl)\n"
	"    try(pl.GetNumUnits, pl) try(pl.GetNumMilitaryUnits, pl) try(pl.GetScore, pl) try(pl.GetCapitalCity, pl) try(pl.GetLeaderType, pl) try(pl.GetNumResourceAvailable, pl, 0, true)\n"
	"    try(pl.GetStrategicResourceCount, pl) try(pl.IsTurnActive, pl) try(pl.HasReceivedNetTurnComplete, pl) try(pl.GetNumPlots, pl) try(pl.GetFaith, pl) try(pl.GetTotalFaithPerTurn, pl)\n"
	"    try(pl.GetLuxuryHappiness, pl) try(pl.GetNumGreatPeople, pl) try(pl.GetCultureCityBonus, pl) try(pl.GetInternationalTradeRoutesAvailable, pl) try(pl.GetMinorCivFriendshipWithMajor, pl, 0)\n"
	"    for city in pl:Cities() do\n"
	"      try(city.GetName, city) try(city.GetPopulation, city) try(city.GetX, city) try(city.GetY, city) try(city.GetOwner, city) try(city.GetID, city) try(city.IsCapital, city)\n"
	"      try(city.GetStrengthValue, city) try(city.GetDamage, city) try(city.GetMaxHitPoints, city) try(city.FoodDifference, city) try(city.GetFood, city) try(city.GetProductionNameKey, city)\n"
	"      try(city.GetProductionTurnsLeft, city) try(city.GetCityGrowth, city) try(city.IsPuppet, city) try(city.IsRazing, city) try(city.IsResistance, city) try(city.GetCityStrategyAI, city)\n"
	"      try(city.GetFaithPerTurn, city) try(city.GetBaseYieldRate, city, 0) try(city.GetYieldPerPopTimes100, city, 0) try(city.IsProductionProcess, city) try(city.GetOrderQueueLength, city)\n"
	"      try(city.CanTrain, city, 0) try(city.GetCurrentProductionDifference, city, false, false) try(city.Plot, city) try(city.GetNumBuildings, city) try(city.GetHappinessFromBuildings, city)\n"
	"    end\n"
	"    for unit in pl:Units() do\n"
	"      try(unit.GetID, unit) try(unit.GetX, unit) try(unit.GetY, unit) try(unit.GetUnitType, unit) try(unit.GetOwner, unit) try(unit.GetDamage, unit) try(unit.GetMoves, unit) try(unit.MovesLeft, unit)\n"
	"      try(unit.IsCombatUnit, unit) try(unit.IsEmbarked, unit) try(unit.GetFortifyTurns, unit) try(unit.IsFortifiedThisTurn, unit) try(unit.GetBaseCombatStrength, unit)\n"
	"      try(unit.GetMaxMoves, unit) try(unit.CanMove, unit) try(unit.IsReadyToMove, unit) try(unit.IsAutomated, unit) try(unit.GetActivityType, unit) try(unit.GetLevel, unit) try(unit.GetExperience, unit)\n"
	"      try(unit.IsInvisible, unit, Game.GetActiveTeam(), false) try(unit.IsSelected, unit) try(unit.CanFound, unit, unit:GetPlot()) try(unit.IsFound, unit) try(unit.IsWork, unit) try(unit.GetBuildType, unit)\n"
	"      try(unit.GetPlot, unit) try(unit.GetUnitCombatType, unit) try(unit.GetDomainType, unit) try(unit.IsGarrisoned, unit) try(unit.GetName, unit) try(unit.GetNameNoDesc, unit)\n"
	"    end\n"
	"  end\n"
	"end\n"
	"local w, h = Map.GetGridSize()\n"
	"for x = 0, w - 1, 3 do for y = 0, h - 1, 3 do\n"
	"  local pt = Map.GetPlot(x, y)\n"
	"  if pt then\n"
	"    try(pt.GetOwner, pt) try(pt.GetPlotType, pt) try(pt.GetTerrainType, pt) try(pt.GetFeatureType, pt) try(pt.GetResourceType, pt, Game.GetActiveTeam()) try(pt.IsVisible, pt, Game.GetActiveTeam(), false)\n"
	"    try(pt.IsRevealed, pt, Game.GetActiveTeam(), false) try(pt.GetImprovementType, pt) try(pt.GetRouteType, pt) try(pt.IsCity, pt) try(pt.GetNumUnits, pt) try(pt.IsRiver, pt) try(pt.IsWater, pt)\n"
	"    try(pt.IsMountain, pt) try(pt.GetWorkingCity, pt) try(pt.GetPlotCity, pt) try(pt.GetArea, pt) try(pt.IsHills, pt) try(pt.GetActiveFogOfWarMode, pt) try(pt.IsFriendlyTerritory, pt, me)\n"
	"    try(pt.GetNumResource, pt) try(pt.IsLake, pt) try(pt.GetContinentArtType, pt) try(pt.GetYield, pt, 0)\n"
	"  end\n"
	"end end\n"
	"GetterCalls, GetterErrors, GetterMissing = calls, errors, missing\n";

// The fork's replica is "ready" for the UI when the seat's notifications and units are known to the UI: used only for the report.
struct UnitCount
{
	int units, embarked, cities;
	UnitCount() : units(0), embarked(0), cities(0) {}
};

UnitCount CountLive()
{
	UnitCount c;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isAlive()) continue;
		int loop = 0;
		for (CvCity* city = p.firstCity(&loop); city; city = p.nextCity(&loop)) ++c.cities;
		for (CvUnit* u = p.firstUnit(&loop); u; u = p.nextUnit(&loop))
		{
			++c.units;
			if (u->isEmbarked()) ++c.embarked;
		}
	}
	return c;
}

}  // namespace

extern "C" int rt_selftest_replica_ui(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, const char* world_size, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = *host;
	mine.user = &seen;
	mine.log = OnLog;
	mine.read_file = OnReadFile;
	mine.game_event = OnEvent;
	gc_error err;
	memset(&err, 0, sizeof(err));
	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);
	if (!inst)
	{
		snprintf(report, report_len, "{\"checks\":[%s]}", c.json.c_str());
		return 1;
	}

	// ---- the server plays ----
	std::string players = "{\"kind\":\"human\",\"civ\":\"CIVILIZATION_ROME\",\"nick\":\"bot0\"},{\"kind\":\"human\",\"civ\":\"random\",\"nick\":\"bot1\"}";
	for (unsigned i = 0; i < ai; ++i) players += ",{\"kind\":\"ai\",\"civ\":\"random\"}";
	const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"autoplay_humans\":[0,1],\"handicap\":\"HANDICAP_CHIEFTAIN\",\"world_size\":\"%s\"}", seed,
		players.c_str(), world_size && *world_size ? world_size : "WORLDSIZE_TINY");
	int32_t rc = gc_new_game(inst, setup.c_str(), &err);
	c.add("new_game", rc == GC_OK, "gc_new_game: %d %s", err.code, err.message);
	if (rc == GC_OK) rc = gc_set_role(inst, GC_ROLE_SERVER, -1, &err);
	if (rc != GC_OK)
	{
		c.add("server_role", false, "gc_set_role: %d %s", err.code, err.message);
		snprintf(report, report_len, "{\"checks\":[%s]}", c.json.c_str());
		gc_destroy(inst);
		return 1;
	}
	std::vector<Batch> batches;
	std::vector<uint8_t> first, last;
	int turn = 0;
	std::string why;
	double t0 = NowMs();
	bool ok = PumpToSavePoint(inst, -1, batches, &turn, first, why);
	const int startTurn = turn;
	const size_t firstBatches = batches.size();
	const uint64_t startDigest = Digest(inst, -3);
	last = first;
	// The batches the replica applies are the ones after the first snapshot.
	std::vector<Batch> journal;
	if (ok)
	{
		while (ok && turn < startTurn + (int)turns)
		{
			std::vector<Batch> chunk;
			ok = PumpToSavePoint(inst, turn, chunk, &turn, last, why);
			for (size_t i = 0; i < chunk.size(); ++i) journal.push_back(chunk[i]);
		}
	}
	const uint64_t endDigest = Digest(inst, -3);
	c.add("server_play", ok && !journal.empty(), "%d turns from %d, %u pump batches in %.0f ms%s%s", (int)turns, startTurn, (unsigned)journal.size(), NowMs() - t0, why.empty() ? "" : ": ", why.c_str());
	(void)firstBatches;
	if (!ok)
	{
		snprintf(report, report_len, "{\"checks\":[%s]}", c.json.c_str());
		gc_destroy(inst);
		return 1;
	}

	// ---- the same instance as a replica of seat 0 ----
	Sink sink;
	gc_ui_sink usink;
	usink.user = &sink;
	usink.event = OnUiEvent;
	rc = gc_set_ui_sink(inst, &usink, &err);
	c.add("sink_set", rc == GC_OK, "gc_set_ui_sink: %d %s", err.code, err.message);
	rc = gc_set_role(inst, GC_ROLE_REPLICA, 0, &err);
	c.add("replica_role", rc == GC_OK, "gc_set_role(REPLICA, 0): %d %s", err.code, err.message);
	rc = gc_resync(inst, &first[0], (uint32_t)first.size(), &err);
	c.add("resync_first", rc == GC_OK, "gc_resync: %d %s", err.code, err.message);
	c.add("digest_first", Digest(inst, -3) == startDigest, "replica %s, server %s", Hex(Digest(inst, -3)).c_str(), Hex(startDigest).c_str());
	c.add("active_is_seat", CvPreGame::activePlayer() == 0 && GC.getGame().getActivePlayer() == 0, "the active player at rest is %d", (int)CvPreGame::activePlayer());
	c.add("no_events_from_resync", sink.events.empty(), "%u event(s) from the resync (it is muted)", (unsigned)sink.events.size());

	lua_State* U = luaL_newstate();
	luaL_openlibs(U);
	const int top0 = lua_gettop(U);
	rc = gc_lua_attach_ui(inst, U, &err);
	c.add("attach_ui", rc == GC_OK, "gc_lua_attach_ui: %d %s", err.code, err.message);
	c.add("attach_stack", lua_gettop(U) == top0, "stack %d before, %d after", top0, lua_gettop(U));
	// A second attach is harmless (hosts attach again to a rebuilt state).
	{
		const int top = lua_gettop(U);
		const int32_t rc2 = gc_lua_attach_ui(inst, U, &err);
		c.add("attach_twice", rc2 == GC_OK && lua_gettop(U) == top, "second attach: %d", rc2);
	}
	{
		bool lok = false;
		const std::string name = EvalLua(U, "Players[0]:GetName()", &lok);
		c.add("lua_player_name", lok && !name.empty() && name != "nil", "Players[0]:GetName() = '%s'", name.c_str());
		const std::string active = EvalLua(U, "Game.GetActivePlayer()", &lok);
		c.add("lua_active_player", lok && active == "0", "Game.GetActivePlayer() = %s", active.c_str());
		const std::string units = EvalLua(U, "(function() local n = 0 for u in GameInfo.Units() do n = n + 1 end return n end)()", &lok);
		c.add("lua_gameinfo_units", lok && atoi(units.c_str()) >= 50, "GameInfo.Units(): %s rows", units.c_str());
		const std::string tbl = EvalLua(U, "(function() local names = {} for r in DB.Query('PRAGMA table_info(Units)') do names[#names + 1] = r.name end return #names .. ':' .. names[1] end)()", &lok);
		c.add("lua_db_pragma", lok && atoi(tbl.c_str()) > 10, "PRAGMA table_info(Units): %s", tbl.c_str());
		const std::string defines = EvalLua(U, "GameDefines.MAX_MAJOR_CIVS", &lok);
		c.add("lua_game_defines", lok && atoi(defines.c_str()) > 0, "GameDefines.MAX_MAJOR_CIVS = %s", defines.c_str());
		const std::string pre = EvalLua(U, "PreGame.GetNumMinorCivs ~= nil and PreGame.IsMultiplayerGame()", &lok);
		c.add("lua_pregame", lok && pre == "true", "PreGame.IsMultiplayerGame() = %s", pre.c_str());
		const std::string nilm = EvalLua(U, "(Map.GetPlot(0, 0).GetCulture == nil) and (Map.GetPlot(0, 0).GetOwner ~= nil) and (Players[0].GetNumCities ~= nil)", &lok);
		c.add("lua_missing_methods_are_nil", lok && nilm == "true", "Plot.GetCulture == nil: %s", nilm.c_str());
		const std::string orbit = EvalLua(U, "(function() for u in Players[0]:Units() do return u.IsInOrbit == nil and u.GetID ~= nil end return 'no units' end)()", &lok);
		c.add("lua_unit_methods", lok && orbit == "true", "Unit.IsInOrbit == nil: %s", orbit.c_str());
		const std::string conn = EvalLua(U, "Players[0].GetCityConnectionRouteGoldTimes100 ~= nil", &lok);
		printf("replicaui: info Player.GetCityConnectionRouteGoldTimes100 ~= nil: %s\n", conn.c_str());
	}

	// ---- the graphical setup ----
	gc_ui_subscribe_game_event(inst, "PlayerAdoptPolicy", &err);
	gc_ui_subscribe_game_event(inst, "PlayerDoTurn", &err);
	rc = gc_ui_setup_graphical(inst, &err);
	c.add("setup_graphical", rc == GC_OK, "gc_ui_setup_graphical: %d %s", err.code, err.message);
	{
		const UnitCount live = CountLive();
		std::set<std::pair<int, int> > createdUnits, createdCities;
		int rebuilt = 0, unitDup = 0;
		for (size_t i = 0; i < sink.events.size(); ++i)
		{
			const Rec& e = sink.events[i];
			if (e.flags & GC_UI_F_REBUILD) ++rebuilt;
			if ((e.kind == GC_UI_EV_UNIT_CREATED || e.kind == GC_UI_EV_UNIT_EMBARK) && e.args.size() >= 2)
			{
				if (e.kind == GC_UI_EV_UNIT_EMBARK && createdUnits.count(std::make_pair(e.args[0], e.args[1]))) continue;
				if (!createdUnits.insert(std::make_pair(e.args[0], e.args[1])).second) ++unitDup;
			}
			if (e.kind == GC_UI_EV_CITY_CREATED && e.args.size() >= 2) createdCities.insert(std::make_pair(e.args[0], e.args[1]));
		}
		c.add("setup_units", (int)createdUnits.size() == live.units && unitDup == 0, "%d living unit(s) (%d embarked), %d announced, %d announced twice", live.units, live.embarked, (int)createdUnits.size(), unitDup);
		c.add("setup_cities", (int)createdCities.size() == live.cities && sink.Count(GC_UI_EV_CITY_CREATED) == live.cities, "%d living city(ies), %d announced (%d CITY_CREATED events)", live.cities, (int)createdCities.size(),
			sink.Count(GC_UI_EV_CITY_CREATED));
		c.add("setup_rebuild_flag", rebuilt == (int)sink.events.size() && rebuilt > 0, "%d of %u events carry REBUILD", rebuilt, (unsigned)sink.events.size());
		c.add("setup_fog", sink.Count(GC_UI_EV_FOW_CHANGED) > 0, "%d FOW_CHANGED event(s)", sink.Count(GC_UI_EV_FOW_CHANGED));
		c.add("digest_after_setup", Digest(inst, -3) == startDigest, "the graphical setup leaves the replica digest alone");
		// The packed unit: owner, id, x, y, type, ..., fog, selected, military, visible.
		bool shape = false;
		for (size_t i = 0; i < sink.events.size() && !shape; ++i)
			if (sink.events[i].kind == GC_UI_EV_UNIT_CREATED) shape = sink.events[i].args.size() == (size_t)GC_UI_UNIT_INTS;
		c.add("unit_event_shape", shape, "UNIT_CREATED carries %d ints", GC_UI_UNIT_INTS);
	}

	// ---- the journal, batch by batch, with the UI reading between ----
	sink.Clear();
	int getterRuns = 0, digestBad = 0;
	long getterCalls = 0, getterErrors = 0;
	std::string firstBad, firstGetterError;
	double applyMs = 0, getterMs = 0;
	bool applied = true;
	for (size_t i = 0; i < journal.size() && applied; ++i)
	{
		double t = NowMs();
		rc = gc_journal_apply(inst, journal[i].bytes.empty() ? NULL : &journal[i].bytes[0], (uint32_t)journal[i].bytes.size(), &err);
		applyMs += NowMs() - t;
		if (rc != GC_OK) { applied = false; c.add("journal_apply", false, "batch %u of %u: %d %s", (unsigned)i, (unsigned)journal.size(), err.code, err.message); break; }
		t = NowMs();
		const std::string e = RunLua(U, kGetterScript, "=getters");
		getterMs += NowMs() - t;
		++getterRuns;
		if (!e.empty() && firstGetterError.empty()) firstGetterError = e;
		getterCalls += atol(EvalLua(U, "GetterCalls").c_str());
		getterErrors += atol(EvalLua(U, "GetterErrors").c_str());
		if (firstGetterError.empty() && getterErrors > 0) firstGetterError = EvalLua(U, "LastGetterError");
		if (!journal[i].bytes.empty() && journal[i].digest != 0)
		{
			const uint64_t mine = Digest(inst, -3);
			if (mine != journal[i].digest) { ++digestBad; if (firstBad.empty()) firstBad = Format("batch %u: replica %s, server %s", (unsigned)i, Hex(mine).c_str(), Hex(journal[i].digest).c_str()); }
		}
	}
	if (applied) c.add("journal_apply", true, "%u batches in %.0f ms", (unsigned)journal.size(), applyMs);
	c.add("digest_every_batch", applied && digestBad == 0, "%d batch(es) differ from the server's digest%s%s", digestBad, firstBad.empty() ? "" : ": ", firstBad.c_str());
	c.add("digest_end", Digest(inst, -3) == endDigest, "replica %s, server %s", Hex(Digest(inst, -3)).c_str(), Hex(endDigest).c_str());
	c.add("getters_ran", getterRuns > 0 && getterCalls > 100 * getterRuns / 2, "%d runs, %ld calls, %.1f ms of Lua", getterRuns, getterCalls, getterMs);
	printf("replicaui: info %s getter call(s) per run named a method this DLL does not have (nil, skipped)\n", EvalLua(U, "GetterMissing").c_str());
	c.add("getters_no_error", getterErrors == 0,"%ld getter error(s)%s%s", getterErrors, firstGetterError.empty() ? "" : ": ", firstGetterError.c_str());
	{
		static const int kinds[] = {GC_UI_EV_UNIT_MOVED, GC_UI_EV_UNIT_TELEPORTED, GC_UI_EV_NOTIFICATION_ADDED, GC_UI_EV_SET_DIRTY, GC_UI_EV_ACTIVE_PLAYER_TURN_START, GC_UI_EV_TURN_CHANGED, GC_UI_EV_FOW_CHANGED,
			GC_UI_EV_UNIT_CREATED, GC_UI_EV_CITY_CREATED, GC_UI_EV_GAME_EVENT};
		std::string line;
		for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) line += Format("%s%d:%d", i ? " " : "", kinds[i], sink.Count(kinds[i]));
		printf("replicaui: info events during the journal (kind:count): %s; %u in all\n", line.c_str(), (unsigned)sink.events.size());
		c.add("events_flow", sink.Count(GC_UI_EV_SET_DIRTY) > 0 && sink.Count(GC_UI_EV_NOTIFICATION_ADDED) > 0, "SET_DIRTY %d, NOTIFICATION_ADDED %d", sink.Count(GC_UI_EV_SET_DIRTY), sink.Count(GC_UI_EV_NOTIFICATION_ADDED));
		int policies = 0;
		for (size_t i = 0; i < sink.events.size(); ++i)
			if (sink.events[i].kind == GC_UI_EV_GAME_EVENT && sink.events[i].text == "PlayerAdoptPolicy") ++policies;
		int turnDo = 0;
		for (size_t i = 0; i < sink.events.size(); ++i)
			if (sink.events[i].kind == GC_UI_EV_GAME_EVENT && sink.events[i].text == "PlayerDoTurn") ++turnDo;
		if (turns >= 30) c.add("game_event_policy", policies > 0, "GameEvents.PlayerAdoptPolicy came out of the sink %d time(s)", policies);
		else printf("replicaui: info GameEvents.PlayerAdoptPolicy %d time(s), PlayerDoTurn %d time(s) (turns < 30: not required)\n", policies, turnDo);
		c.add("game_event_do_turn", turnDo > 0, "GameEvents.PlayerDoTurn %d time(s); the data blob is a count and typed values", turnDo);
		bool blobOk = false;
		for (size_t i = 0; i < sink.events.size() && !blobOk; ++i)
			if (sink.events[i].kind == GC_UI_EV_GAME_EVENT && sink.events[i].text == "PlayerDoTurn")
				blobOk = !sink.events[i].data.empty() && sink.events[i].data[0] >= 1 && sink.events[i].data.size() >= 10 && sink.events[i].data[1] == GC_UI_VAL_NUM;
		c.add("game_event_values", blobOk, "the first PlayerDoTurn carries its player as a number");
	}

	// ---- the interface state ----
	{
		sink.Clear();
		int32_t out[64];
		uint32_t n = 64;
		int ownerId[2] = {-1, -1};
		{
			int loop = 0;
			CvPlayer& p = GET_PLAYER((PlayerTypes)0);
			CvUnit* u = p.firstUnit(&loop);
			if (u) { ownerId[0] = (int)u->getOwner(); ownerId[1] = u->GetID(); }
		}
		rc = gc_ui_op(inst, GC_UI_OP_SELECT_UNIT, ownerId, 2, NULL, NULL, &err);
		c.add("op_select_unit", rc == GC_OK && sink.Count(GC_UI_EV_UNIT_SELECTION_CHANGED) == 1, "SELECT_UNIT (%d, %d): %d, %d UNIT_SELECTION_CHANGED", ownerId[0], ownerId[1], rc, sink.Count(GC_UI_EV_UNIT_SELECTION_CHANGED));
		n = 64;
		rc = gc_ui_op(inst, GC_UI_OP_HEAD_SELECTED_UNIT, NULL, 0, out, &n, &err);
		c.add("op_head_selected", rc == GC_OK && n == 2 && out[0] == ownerId[0] && out[1] == ownerId[1], "HEAD_SELECTED_UNIT = (%d, %d)", n >= 2 ? (int)out[0] : -9, n >= 2 ? (int)out[1] : -9);
		const std::string sel = EvalLua(U, "(function() for u in Players[0]:Units() do if u:IsSelected() then return u:GetID() end end return 'none' end)()");
		c.add("op_selection_seen_by_lua", atoi(sel.c_str()) == ownerId[1] && sel != "none", "Unit:IsSelected() true for unit %s", sel.c_str());
		int32_t mode = INTERFACEMODE_MOVE_TO;
		rc = gc_ui_op(inst, GC_UI_OP_SET_INTERFACE_MODE, &mode, 1, NULL, NULL, &err);
		n = 64;
		gc_ui_op(inst, GC_UI_OP_GET_INTERFACE_MODE, NULL, 0, out, &n, &err);
		c.add("op_interface_mode", rc == GC_OK && n == 1 && out[0] == (int32_t)INTERFACEMODE_MOVE_TO && sink.Count(GC_UI_EV_INTERFACE_MODE_CHANGED) == 1, "SET/GET_INTERFACE_MODE: %d, mode %d", rc, n ? (int)out[0] : -9);
		mode = INTERFACEMODE_SELECTION;
		gc_ui_op(inst, GC_UI_OP_SET_INTERFACE_MODE, &mode, 1, NULL, NULL, &err);
		n = 64;
		rc = gc_ui_op(inst, GC_UI_OP_CAN_END_TURN, NULL, 0, out, &n, &err);
		c.add("op_can_end_turn", rc == GC_OK && n == 1, "CAN_END_TURN = %d", n ? (int)out[0] : -9);
		n = 64;
		rc = gc_ui_op(inst, GC_UI_OP_CAN_SELECTION_LIST_FOUND, NULL, 0, out, &n, &err);
		c.add("op_can_found", rc == GC_OK && n == 1, "CAN_SELECTION_LIST_FOUND = %d", n ? (int)out[0] : -9);
		rc = gc_ui_op(inst, GC_UI_OP_CLEAR_SELECTION, NULL, 0, NULL, NULL, &err);
		n = 64;
		gc_ui_op(inst, GC_UI_OP_GET_SELECTION_COUNT, NULL, 0, out, &n, &err);
		c.add("op_clear_selection", rc == GC_OK && n == 1 && out[0] == 0, "CLEAR_SELECTION: %d selected", n ? (int)out[0] : -9);
		// a city
		int cityId[2] = {-1, -1};
		{
			int loop = 0;
			CvCity* city = GET_PLAYER((PlayerTypes)0).firstCity(&loop);
			if (city) { cityId[0] = 0; cityId[1] = city->GetID(); }
		}
		if (cityId[1] >= 0)
		{
			sink.Clear();
			rc = gc_ui_op(inst, GC_UI_OP_SELECT_CITY, cityId, 2, NULL, NULL, &err);
			n = 64;
			gc_ui_op(inst, GC_UI_OP_HEAD_SELECTED_CITY, NULL, 0, out, &n, &err);
			c.add("op_select_city", rc == GC_OK && n == 2 && out[1] == cityId[1] && sink.Count(GC_UI_EV_CITY_SELECTION_CHANGED) == 1, "SELECT_CITY %d: head (%d, %d)", cityId[1], n >= 2 ? (int)out[0] : -9, n >= 2 ? (int)out[1] : -9);
			int32_t up = 1;
			gc_ui_op(inst, GC_UI_OP_SET_CITY_SCREEN_UP, &up, 1, NULL, NULL, &err);
			n = 64;
			gc_ui_op(inst, GC_UI_OP_IS_CITY_SCREEN_UP, NULL, 0, out, &n, &err);
			c.add("op_city_screen", n == 1 && out[0] == 1 && sink.Count(GC_UI_EV_CITY_SCREEN_CHANGED) == 1, "city screen up: %d", n ? (int)out[0] : -9);
			up = 0;
			gc_ui_op(inst, GC_UI_OP_SET_CITY_SCREEN_UP, &up, 1, NULL, NULL, &err);
			gc_ui_op(inst, GC_UI_OP_CLEAR_SELECTED_CITIES, NULL, 0, NULL, NULL, &err);
		}
		else
			printf("replicaui: info seat 0 has no city yet: city ops skipped\n");
		// a notification of the seat
		int notif = -1;
		for (size_t i = 0; i < sink.events.size(); ++i) {}
		(void)notif;
		int32_t bad[2] = {-1, -1};
		rc = gc_ui_op(inst, GC_UI_OP_SELECT_UNIT, bad, 2, NULL, NULL, &err);
		c.add("op_not_found", rc == GC_ERR_NOT_FOUND, "SELECT_UNIT of a unit that does not exist: %d", rc);
		rc = gc_ui_op(inst, 999, NULL, 0, NULL, NULL, &err);
		c.add("op_unknown", rc == GC_ERR_INVALID_ARG, "an unknown op: %d", rc);
		c.add("digest_after_ops", Digest(inst, -3) == endDigest, "the interface ops leave the replica digest alone");
	}

	// ---- a resync and the invalidation ----
	{
		const std::string held = EvalLua(U, "(function() for u in Players[0]:Units() do OLD_UNIT = u OLD_PLOT = u:GetPlot() return u:GetID() end return 'none' end)()");
		const std::string cityHeld = EvalLua(U, "(function() for city in Players[0]:Cities() do OLD_CITY = city return city:GetID() end return 'none' end)()");
		printf("replicaui: info held unit %s city %s\n", held.c_str(), cityHeld.c_str());
		// addresses before: units and plots of the game
		void* plot0 = GC.getMap().plotByIndex(0);
		CvUnit* unit0 = NULL;
		{
			int loop = 0;
			unit0 = GET_PLAYER((PlayerTypes)0).firstUnit(&loop);
		}
		sink.Clear();
		rc = gc_resync(inst, &last[0], (uint32_t)last.size(), &err);
		c.add("resync_last", rc == GC_OK, "gc_resync: %d %s", err.code, err.message);
		void* plot0After = GC.getMap().plotByIndex(0);
		CvUnit* unit0After = NULL;
		{
			int loop = 0;
			unit0After = GET_PLAYER((PlayerTypes)0).firstUnit(&loop);
		}
		printf("replicaui: info after a resync: plot 0 %s, first unit of seat 0 %s\n", plot0 == plot0After ? "same address" : "moved", unit0 == unit0After ? "same address" : "moved");
		c.add("resync_muted", sink.events.empty(), "%u event(s) from the resync", (unsigned)sink.events.size());
		c.add("digest_resync", Digest(inst, -3) == endDigest, "replica %s, server %s", Hex(Digest(inst, -3)).c_str(), Hex(endDigest).c_str());
		c.add("active_after_resync", CvPreGame::activePlayer() == 0, "the active player at rest is %d", (int)CvPreGame::activePlayer());
		rc = gc_lua_invalidate_ui(inst, U, &err);
		c.add("invalidate", rc == GC_OK && lua_gettop(U) == top0, "gc_lua_invalidate_ui: %d %s, stack %d", rc, err.message, lua_gettop(U));
		bool lok = false;
		if (held != "none")
		{
			const std::string r = EvalLua(U, "(function() local ok, e = pcall(OLD_UNIT.GetID, OLD_UNIT) return tostring(ok) .. ':' .. tostring(e) end)()", &lok);
			c.add("old_unit_is_an_error", lok && r.compare(0, 6, "false:") == 0, "OLD_UNIT:GetID() -> %s", r.c_str());
		}
		if (cityHeld != "none")
		{
			const std::string r = EvalLua(U, "(function() local ok, e = pcall(OLD_CITY.GetID, OLD_CITY) return tostring(ok) .. ':' .. tostring(e) end)()", &lok);
			c.add("old_city_is_an_error", lok && r.compare(0, 6, "false:") == 0, "OLD_CITY:GetID() -> %s", r.c_str());
		}
		const std::string name = EvalLua(U, "Players[0]:GetName()", &lok);
		c.add("players_survive_resync", lok && !name.empty() && name != "nil", "Players[0]:GetName() = '%s'", name.c_str());
		const std::string fresh = EvalLua(U, "(function() local n = 0 for u in Players[0]:Units() do n = n + u:GetID() * 0 + 1 end return n end)()", &lok);
		c.add("fresh_units_work", lok && atoi(fresh.c_str()) > 0, "Players[0]:Units(): %s unit(s)", fresh.c_str());
		// the setup again: the same announcements
		const int u0 = sink.Count(GC_UI_EV_UNIT_CREATED) + sink.Count(GC_UI_EV_UNIT_EMBARK);
		rc = gc_ui_setup_graphical(inst, &err);
		const UnitCount live = CountLive();
		c.add("setup_after_resync", rc == GC_OK && sink.Count(GC_UI_EV_UNIT_CREATED) + sink.Count(GC_UI_EV_UNIT_EMBARK) - u0 == live.units && sink.Count(GC_UI_EV_CITY_CREATED) == live.cities,
			"%d unit(s) and %d city(ies) announced for %d and %d alive", sink.Count(GC_UI_EV_UNIT_CREATED) + sink.Count(GC_UI_EV_UNIT_EMBARK) - u0, sink.Count(GC_UI_EV_CITY_CREATED), live.units, live.cities);
		const std::string again = RunLua(U, kGetterScript, "=getters");
		c.add("getters_after_resync", again.empty() && EvalLua(U, "GetterErrors") == "0", "%s errors %s", again.c_str(), EvalLua(U, "GetterErrors").c_str());
	}

	// ---- the rules refuse the replica's own moves; a server role has no events ----
	{
		gc_pump_result pr;
		rc = gc_pump(inst, 1, &pr, &err);
		c.add("replica_pump_refused", rc == GC_ERR_REJECTED, "gc_pump on a replica: %d", rc);
		rc = gc_ui_op(inst, GC_UI_OP_CLEAR_SELECTION, NULL, 0, NULL, NULL, &err);
		gc_set_role(inst, GC_ROLE_PLAIN, -1, &err);
		sink.Clear();
		rc = gc_ui_op(inst, GC_UI_OP_CLEAR_SELECTION, NULL, 0, NULL, NULL, &err);
		c.add("plain_has_no_ui", rc == GC_ERR_REJECTED, "gc_ui_op outside the replica role: %d", rc);
		c.add("plain_active_is_server_seat", CvPreGame::activePlayer() != 0 || ServerSeat() == 0, "active %d, server seat %d", (int)CvPreGame::activePlayer(), (int)ServerSeat());
	}

	c.add("lua_errors", seen.luaErrors == 0, "%d Lua error event(s) from the core", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d log error(s)%s%s", seen.logErrors, seen.lines.empty() ? "" : ": ", seen.lines.empty() ? "" : seen.lines[0].c_str());
	lua_close(U);
	gc_destroy(inst);
	snprintf(report, report_len, "{\"seed\":%u,\"turns\":%u,\"failed\":%d,\"checks\":[%s]}", seed, turns, c.failed, c.json.c_str());
	return c.failed == 0 ? 0 : 1;
}
