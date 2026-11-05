// Scenario `purity` (AC-N1.4, task N3.15; run.sh --scenario purity --humans 2 --ai 2 --turns 50): the queries and the refused commands do not
// change the simulation. A network game like `mp` (test-bot humans through gc_pump), one process per run; run.sh runs three of the same seed and compares the
// "purity-digest|turn|hex" lines (gc_state_digest(-2), the simulation digest, after every turn change) turn by turn:
//   --noise 0  nothing else: the reference;
//   --noise 1  between the gc_pump calls 10 000 random queries Q_* 1..21 for every human (kind, player, unit, city, plot, viewer by a generator with its own RNG: not the
//              bot and not the game's RNG; about one in five with arguments that must be refused), 200 a turn (also the ones the bot's own queries do not make);
//   --noise 2  between the calls 5 000 random commands that must be refused (a wrong player, a unit of another human, a plot off the map, a wrong argument count, an unknown
//              kind, ...); a command that is accepted is a failure of the scenario (the generator makes only commands the checks must refuse) and a refusal must change nothing.
// Prints "purity-digest|turn|hex", "purity-noise|..." and "purity-kind|..." lines; the checks of one run are the turns, STUCK, errors, the counts.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <map>
#include <string>
#include <vector>

#include "../abi/gc_command.h"
#include "../abi/gc_internal.h"
#include "rt_active.h"
#include "rt_selftest.h"
#include "rt_session.h"

using namespace lekmod_portable;

namespace {

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

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

std::string Num(long long v) { return Format("%lld", v); }

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
		printf("purity: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings, luaErrors;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0) {}
};

void OnLog(void* user, int32_t level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == GC_LOG_ERROR) { ++s->logErrors; if (s->lines.size() < 20) s->lines.push_back(std::string("log error: ") + msg); }
	if (level == GC_LOG_WARN) ++s->logWarnings;
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

// The generator's own RNG (xorshift64*): neither the bot's nor the game's.
struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
	uint64_t Next()
	{
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 2685821657736338717ull;
	}
	int Below(int n) { return n <= 1 ? 0 : (int)(Next() % (uint64_t)n); }
	bool Chance(int percent) { return Below(100) < percent; }
};

// What the generator may pick from, read straight from the gamecore without side effects (the unit and city lists of the humans, the map size).
struct World
{
	std::vector<int> humans;
	std::vector<std::vector<int> > units, cities;  // per human slot index
	int width, height, plots;
	void Refresh()
	{
		units.assign(humans.size(), std::vector<int>());
		cities.assign(humans.size(), std::vector<int>());
		for (size_t h = 0; h < humans.size(); ++h)
		{
			CvPlayer& p = GET_PLAYER((PlayerTypes)humans[h]);
			int loop;
			for (const CvUnit* u = p.firstUnit(&loop); u; u = p.nextUnit(&loop)) units[h].push_back(u->GetID());
			for (const CvCity* c = p.firstCity(&loop); c; c = p.nextCity(&loop)) cities[h].push_back(c->GetID());
		}
		width = GC.getMap().getGridWidth();
		height = GC.getMap().getGridHeight();
		plots = GC.getMap().numPlots();
	}
};

struct Noise
{
	long long done, ok, refused, other, unexpectedOk;
	std::map<int, int> rcs;
	std::map<int, int> perKind, okPerKind;
	std::string firstUnexpected;
	Noise() : done(0), ok(0), refused(0), other(0), unexpectedOk(0) {}
};

// One random query. `bad` makes the arguments (probably) wrong. The reply is only released: what counts is what the call does to the game.
void OneQuery(gc_instance* inst, Rng& rng, const World& w, Noise& n, const std::vector<int>& only)
{
	const int kind = only.empty() ? 1 + rng.Below(21) : only[(size_t)rng.Below((int)only.size())];
	const bool bad = rng.Chance(20);
	const size_t h = w.humans.empty() ? 0 : (size_t)rng.Below((int)w.humans.size());
	const int player = w.humans.empty() ? 0 : (bad && rng.Chance(50) ? rng.Below(MAX_PLAYERS + 3) - 1 : w.humans[h]);
	const std::vector<int>& us = w.units.empty() ? std::vector<int>() : w.units[h];
	const std::vector<int>& cs = w.cities.empty() ? std::vector<int>() : w.cities[h];
	const int unit = us.empty() || (bad && rng.Chance(50)) ? rng.Below(100000) : us[(size_t)rng.Below((int)us.size())];
	const int city = cs.empty() || (bad && rng.Chance(50)) ? rng.Below(100000) : cs[(size_t)rng.Below((int)cs.size())];
	const int viewer = bad && rng.Chance(50) ? rng.Below(MAX_TEAMS + 4) - 2 : rng.Below(MAX_TEAMS) - (rng.Chance(10) ? 1 : 0);
	const int x = rng.Below(w.width + (bad ? 6 : 0)) - (bad && rng.Chance(30) ? 3 : 0), y = rng.Below(w.height + (bad ? 6 : 0)) - (bad && rng.Chance(30) ? 3 : 0);
	int32_t args[16];
	unsigned argc = 0;
	switch (kind)
	{
	case GC_Q_GAME: if (rng.Chance(60)) args[argc++] = player; break;
	case GC_Q_PLOTS:
		args[argc++] = viewer;
		for (int i = rng.Below(4); i > 0; --i) args[argc++] = rng.Below(w.plots + (bad ? 50 : 0));
		break;
	case GC_Q_UNITS:
	case GC_Q_PLAYERS:
	case GC_Q_CITY_ART:
	case GC_Q_TRADE_ROUTES:
	case GC_Q_TRADE_PATHS: args[argc++] = viewer; break;
	case GC_Q_CITIES:
	case GC_Q_TOP_BAR: args[argc++] = (kind == GC_Q_CITIES ? viewer : player); break;
	case GC_Q_CITY_BUILDS: args[argc++] = player; args[argc++] = city; break;
	case GC_Q_UNIT_ACTIONS:
	case GC_Q_REACHABLE: args[argc++] = player; args[argc++] = unit; break;
	case GC_Q_PATH: args[argc++] = player; args[argc++] = unit; args[argc++] = x; args[argc++] = y; break;
	case GC_Q_RANGE_TARGETS: args[argc++] = player; args[argc++] = rng.Chance(25) ? -city : unit; break;
	case GC_Q_NOTIFICATIONS:
	case GC_Q_DIPLO: args[argc++] = player; break;
	case GC_Q_CATALOG: args[argc++] = rng.Below(GC_CATALOG_TABLE_COUNT + (bad ? 3 : 0)); break;
	case GC_Q_REACHABLE_TREE: args[argc++] = player; args[argc++] = unit; if (rng.Chance(70)) args[argc++] = 1 + rng.Below(bad ? 5 : 3); break;
	case GC_Q_PLOT_BASIS: for (int i = rng.Below(3) == 0 ? 0 : 1 + rng.Below(5); i > 0; --i) args[argc++] = rng.Below(w.plots + (bad ? 50 : 0)); break;
	case GC_Q_COMBAT_PREVIEW: args[argc++] = player; args[argc++] = rng.Chance(25) ? -city : unit; args[argc++] = x; args[argc++] = y; break;
	default: break;  // Q_TURN, Q_PLOTS_DIRTY: no arguments
	}
	if (bad && rng.Chance(15) && argc < 15) args[argc++] = rng.Below(1000);  // a wrong argument count
	gc_buf buf;
	memset(&buf, 0, sizeof(buf));
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t rc = gc_query(inst, kind, args, argc, &buf, &err);
	gc_buf_release(&buf);
	++n.done;
	++n.perKind[kind];
	++n.rcs[rc];
	if (rc == GC_OK) { ++n.ok; ++n.okPerKind[kind]; }
	else if (rc == GC_ERR_INVALID_ARG) ++n.refused;
	else ++n.other;
}

// A unit / city id of human `other` that human `h` has no unit / city with (the ids count per player and can coincide).
bool OtherUnit(const World& w, size_t h, size_t other, Rng& rng, int32_t* out)
{
	if (w.humans.size() < 2 || other == h || w.units[other].empty()) return false;
	const int id = w.units[other][(size_t)rng.Below((int)w.units[other].size())];
	if (GET_PLAYER((PlayerTypes)w.humans[h]).getUnit(id)) return false;
	*out = id;
	return true;
}

bool OtherCity(const World& w, size_t h, size_t other, Rng& rng, int32_t* out)
{
	if (w.humans.size() < 2 || other == h || w.cities[other].empty()) return false;
	const int id = w.cities[other][(size_t)rng.Below((int)w.cities[other].size())];
	if (GET_PLAYER((PlayerTypes)w.humans[h]).getCity(id)) return false;
	*out = id;
	return true;
}

// One random command that the checks must refuse. Returns false when none could be made this time.
bool OneBadCommand(gc_instance* inst, Rng& rng, const World& w, int seat, Noise& n)
{
	const size_t h = w.humans.empty() ? 0 : (size_t)rng.Below((int)w.humans.size());
	const size_t other = w.humans.size() < 2 ? h : (h + 1 + (size_t)rng.Below((int)w.humans.size() - 1)) % w.humans.size();
	int32_t args[8];
	unsigned argc = 0;
	gc_command cmd;
	memset(&cmd, 0, sizeof(cmd));
	const int variant = rng.Below(12);
	int player = w.humans.empty() ? 0 : w.humans[h];
	switch (variant)
	{
	case 0:  // a unit command for a unit of another human
	case 1:
	{
		if (!OtherUnit(w, h, other, rng, &args[0])) return false;
		static const int kinds[] = {GC_CMD_UNIT_FORTIFY, GC_CMD_UNIT_SLEEP, GC_CMD_UNIT_SKIP, GC_CMD_UNIT_DISBAND, GC_CMD_UNIT_WAKE, GC_CMD_UNIT_CANCEL, GC_CMD_UNIT_HEAL, GC_CMD_UNIT_AUTOMATE_EXPLORE};
		cmd.kind = (uint16_t)kinds[rng.Below(8)];
		++argc;
		break;
	}
	case 2:  // a move to a plot off the map
		if (w.units[h].empty()) return false;
		cmd.kind = GC_CMD_UNIT_MOVE_TO;
		args[argc++] = w.units[h][(size_t)rng.Below((int)w.units[h].size())];
		args[argc++] = w.width + rng.Below(20);
		args[argc++] = rng.Below(w.height);
		break;
	case 3:  // a unit that does not exist
		cmd.kind = (uint16_t)(rng.Chance(50) ? GC_CMD_UNIT_FOUND_CITY : GC_CMD_UNIT_FORTIFY);
		args[argc++] = 900000 + rng.Below(1000);
		break;
	case 4:  // a city of another human
		if (!OtherCity(w, h, other, rng, &args[0])) return false;
		cmd.kind = GC_CMD_CITY_SET_PRODUCTION;
		++argc;
		args[argc++] = rng.Below(4);
		args[argc++] = rng.Below(20);
		break;
	case 5:  // a command for a player who is not a human: the server seat, or a slot number outside the table
		player = rng.Chance(50) ? seat : 200 + rng.Below(50);
		cmd.kind = (uint16_t)(rng.Chance(50) ? GC_CMD_END_TURN : GC_CMD_UNIT_SKIP);
		if (cmd.kind == GC_CMD_UNIT_SKIP) args[argc++] = rng.Below(100);
		break;
	case 6:  // war against a team that does not exist, or one's own
		cmd.kind = GC_CMD_DECLARE_WAR;
		args[argc++] = rng.Chance(50) ? MAX_TEAMS + rng.Below(100) : (int)GET_PLAYER((PlayerTypes)player).getTeam();
		break;
	case 7:  // peace with a team that does not exist
		cmd.kind = GC_CMD_MAKE_PEACE;
		args[argc++] = rng.Chance(50) ? MAX_TEAMS + rng.Below(100) : -1 - rng.Below(5);
		break;
	case 8:  // the wrong number of arguments
	{
		static const int kinds[] = {GC_CMD_UNIT_MOVE_TO, GC_CMD_UNIT_FORTIFY, GC_CMD_CITY_SET_PRODUCTION, GC_CMD_UNIT_RANGE_ATTACK, GC_CMD_DECLARE_WAR};
		cmd.kind = (uint16_t)kinds[rng.Below(5)];
		for (int i = rng.Below(2) ? 0 : 7; i > 0; --i) args[argc++] = rng.Below(10);
		break;
	}
	case 9:  // an unknown kind
		cmd.kind = (uint16_t)(rng.Chance(50) ? 99 + rng.Below(900) : 13 + rng.Below(6));
		args[argc++] = rng.Below(10);
		break;
	case 10:  // the timer command from a "human" who is not one
		player = seat;
		cmd.kind = (uint16_t)(rng.Chance(50) ? GC_CMD_END_TURN_TIMEOUT : GC_CMD_CANCEL_END_TURN);
		break;
	default:  // a ranged attack from a unit of another human, a purchase in a city of another human
		if (!OtherUnit(w, h, other, rng, &args[0])) return false;
		cmd.kind = GC_CMD_UNIT_RANGE_ATTACK;
		++argc;
		args[argc++] = rng.Below(w.width);
		args[argc++] = rng.Below(w.height);
		break;
	}
	cmd.player = (uint8_t)player;
	cmd.argc = argc;
	cmd.args = args;
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t rc = gc_apply_command(inst, &cmd, &err);
	++n.done;
	++n.perKind[cmd.kind];
	++n.rcs[rc];
	if (rc == GC_OK)
	{
		++n.ok;
		++n.okPerKind[cmd.kind];
		++n.unexpectedOk;
		if (n.firstUnexpected.empty()) n.firstUnexpected = Format("kind %u player %u argc %u args %d %d %d: accepted", (unsigned)cmd.kind, (unsigned)cmd.player, argc, argc > 0 ? args[0] : 0, argc > 1 ? args[1] : 0, argc > 2 ? args[2] : 0);
	}
	else if (rc == GC_ERR_REJECTED || rc == GC_ERR_INVALID_ARG || rc == GC_ERR_NOT_IMPLEMENTED) ++n.refused;
	else ++n.other;
	return true;
}

// One call of the noise of this run (a command that could not be made is tried again with other dice).
void Fire(gc_instance* inst, int noise, Rng& rng, const World& w, int seat, Noise& n, const std::vector<int>& only)
{
	if (noise == 1) { OneQuery(inst, rng, w, n, only); return; }
	for (int tries = 0; tries < 50 && !OneBadCommand(inst, rng, w, seat, n); ++tries) {}
}

bool SimDigest(gc_instance* inst, uint64_t* out)
{
	gc_error e;
	memset(&e, 0, sizeof(e));
	return gc_state_digest(inst, -2, out, &e) == GC_OK;
}

// noise 3 (bisection of an impure query): the command checks of Q_UNIT_ACTIONS (CheckUnitCommand per unit command kind) for every unit of the humans, one by one, with the simulation
// digest before and after each; a change names the unit and the command kind.
// The serialized objects of a save, each hashed (as SimulationPart does): which one a call changed. FMemoryStream bytes -> FNV-1a.
void ObjectHashes(std::vector<uint64_t>& out, std::vector<std::string>& names)
{
	out.clear();
	names.clear();
	struct Fnv { static uint64_t Of(FMemoryStream& s) { uint64_t h = 1469598103934665603ull; const unsigned char* b = (const unsigned char*)s.GetBuf(); for (size_t i = 0, n = (size_t)s.GetEOF(); i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } return h; } };
	{ FMemoryStream s; GC.getGame().Write(s); out.push_back(Fnv::Of(s)); names.push_back("game"); }
	{ FMemoryStream s; GC.getMap().Write(s); out.push_back(Fnv::Of(s)); names.push_back("map"); }
	for (int i = 0; i < MAX_TEAMS; ++i) { FMemoryStream s; GET_TEAM((TeamTypes)i).Write(s); out.push_back(Fnv::Of(s)); names.push_back(Format("team %d", i)); }
	for (int i = 0; i < MAX_PLAYERS; ++i) { FMemoryStream s; GET_PLAYER((PlayerTypes)i).Write(s); out.push_back(Fnv::Of(s)); names.push_back(Format("player %d", i)); }
	{ FMemoryStream s; GC.getGame().WriteSupportingClassData(s); out.push_back(Fnv::Of(s)); names.push_back("supporting"); }
}

void ProbeUnitChecks(gc_instance* inst, const World& w, int turn, Noise& n, std::vector<std::string>& found)
{
	for (size_t h = 0; h < w.humans.size(); ++h)
	{
		CvPlayer& p = GET_PLAYER((PlayerTypes)w.humans[h]);
		for (size_t i = 0; i < w.units[h].size(); ++i)
		{
			CvUnit* u = p.getUnit(w.units[h][i]);
			if (!u) continue;
			for (int kind = GC_CMD_UNIT_MOVE_TO; kind <= GC_CMD_UNIT_WAKE; ++kind)
			{
				if (!UnitCommandApplies(u, kind)) continue;
				std::vector<uint64_t> before, after;
				std::vector<std::string> names;
				ObjectHashes(before, names);
				std::string why;
				CheckUnitCommand(u, kind, false, -1, -1, why);
				ObjectHashes(after, names);
				++n.done;
				if (before != after)
				{
					++n.other;
					std::string what;
					for (size_t k = 0; k < before.size(); ++k)
						if (before[k] != after[k]) what += Format("%s%s", what.empty() ? "" : ", ", names[k].c_str());
					if (found.size() < 40) found.push_back(Format("turn %d player %d unit %d %s kind %d: changed %s", turn, w.humans[h], u->GetID(), u->getUnitInfo().GetType(), kind, what.c_str()));
				}
			}
		}
	}
}

}  // namespace

extern "C" int rt_selftest_purity(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, int noise, const char* noise_kinds, const char* world_size, char* report, size_t report_len)
{
	Check c;
	std::vector<int> only;  // --noise-kinds 5,6: only these query kinds (bisection)
	for (const char* s = noise_kinds ? noise_kinds : ""; *s;)
	{
		only.push_back(atoi(s));
		while (*s && *s != ',') ++s;
		if (*s == ',') ++s;
	}
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	const long long kQueriesPerTurn = 200, kCommandsPerTurn = 100;
	int startTurn = -1, endTurn = -1, doneTurns = 0, stuck = 0, gameOver = 0, pumpErrors = 0, calls = 0, digestFail = 0;
	long long quotaLeft = 0;
	std::string stopReason;
	uint64_t finalDigest = 0;
	Noise nz;
	std::vector<std::string> probeFound;
	if (inst)
	{
		std::string players;
		for (unsigned i = 0; i < humans; ++i)
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		for (unsigned i = 0; i < ai; ++i) players += std::string(humans || i ? "," : "") + "{\"kind\":\"ai\",\"civ\":\"random\"}";
		std::string slots;
		for (unsigned i = 0; i < humans; ++i) slots += Format("%s%u", i ? "," : "", i);
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"autoplay_humans\":[%s],\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed,
			players.c_str(), slots.c_str(), world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s (noise %d): %d %s", setup.c_str(), noise, rc, err.message);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			World w;
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (IsSeatedHuman(i)) w.humans.push_back(i);
			const int seat = (int)ServerSeat();
			Rng rng(0x5DEECE66Dull * (seed + 1) + (uint64_t)noise);  // the generator's own seed; the digests do not depend on it
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = GC.getGame().getGameTurn();
			quotaLeft = noise == 1 ? kQueriesPerTurn : noise == 2 ? kCommandsPerTurn : 0;
			int turn = startTurn;
			const int target = startTurn + (int)turns;
			const long long maxCalls = 20000LL * (turns + 1);
			while (turn < target)
			{
				if (++calls > maxCalls) { stopReason = Format("turn %d: %lld gc_pump calls without reaching turn %d", turn, maxCalls, target); break; }
				const int32_t prc = gc_pump(inst, 64, &pr, &err);
				if (pr.status == GC_PUMP_STUCK) { ++stuck; stopReason = Format("STUCK at turn %d: %s", pr.turn, err.message); break; }
				if (pr.status == GC_PUMP_GAME_OVER) { ++gameOver; stopReason = Format("GAME_OVER at turn %d", pr.turn); break; }
				if (prc != GC_OK) { ++pumpErrors; stopReason = Format("gc_pump at turn %d: %d %s", pr.turn, prc, err.message); break; }
				if (pr.turn != turn)
				{
					// The rest of the quota of the turn that ended is used up here, so every turn has the same share.
					if (noise && quotaLeft > 0)
					{
						w.Refresh();
						for (; quotaLeft > 0; --quotaLeft) Fire(inst, noise, rng, w, seat, nz, only);
					}
					uint64_t d = 0;
					if (!SimDigest(inst, &d)) ++digestFail;
					printf("purity-digest|%d|%016llx\n", pr.turn, (unsigned long long)d);
					finalDigest = d;
					doneTurns += pr.turn - turn;
					turn = pr.turn;
					quotaLeft = noise == 1 ? kQueriesPerTurn : noise == 2 ? kCommandsPerTurn : 0;
					if (turn >= target) break;
				}
				if (noise == 3)
				{
					w.Refresh();
					ProbeUnitChecks(inst, w, turn, nz, probeFound);
				}
				if ((noise == 1 || noise == 2) && quotaLeft > 0)
				{
					// A few between every two calls, so the noise falls in the AI phase, the human phase and at the turn change alike.
					w.Refresh();
					const int burst = 1 + rng.Below(10);
					for (int i = 0; i < burst && quotaLeft > 0; ++i, --quotaLeft) Fire(inst, noise, rng, w, seat, nz, only);
				}
			}
			endTurn = turn;
		}
	}
	for (size_t i = 0; i < probeFound.size(); ++i) printf("purity-probe|%s\n", probeFound[i].c_str());
	if (noise)
	{
		std::string kinds;
		for (std::map<int, int>::const_iterator it = nz.perKind.begin(); it != nz.perKind.end(); ++it)
		{
			kinds += Format("%s%d:%d", kinds.empty() ? "" : " ", it->first, it->second);
			printf("purity-kind|%s %d|%d call(s), %d accepted\n", noise == 1 ? "Q" : "CMD", it->first, it->second, nz.okPerKind.count(it->first) ? nz.okPerKind[it->first] : 0);
		}
		std::string rcs;
		for (std::map<int, int>::const_iterator it = nz.rcs.begin(); it != nz.rcs.end(); ++it) rcs += Format("%s%d x%d", rcs.empty() ? "" : ", ", it->first, it->second);
		printf("purity-noise|%s|%lld call(s): %lld accepted, %lld refused, %lld other; results: %s\n", noise == 1 ? "queries" : "commands", nz.done, nz.ok, nz.refused, nz.other, rcs.c_str());
	}

	c.add("turns", doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) (turn %d -> %d), %d gc_pump call(s)%s%s", doneTurns, turns, startTurn, endTurn, calls, stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0 && gameOver == 0 && pumpErrors == 0 && digestFail == 0, "%d STUCK, %d GAME_OVER, %d gc_pump error(s), %d digest failure(s)", stuck, gameOver, pumpErrors, digestFail);
	if (noise == 1)
		c.add("queries_made", nz.done >= (long long)turns * kQueriesPerTurn && nz.ok > nz.done / 2 && nz.other == 0,
			"%lld random queries (%lld answered, %lld refused as INVALID_ARG, %lld with another result); %d of 21 kinds asked", nz.done, nz.ok, nz.refused, nz.other, (int)nz.perKind.size());
	if (noise == 2)
		c.add("commands_refused", nz.done >= (long long)turns * kCommandsPerTurn && nz.unexpectedOk == 0 && nz.other == 0,
			"%lld random commands that must be refused: %lld refused, %lld accepted, %lld with another result%s%s", nz.done, nz.refused, nz.unexpectedOk, nz.other,
			nz.firstUnexpected.empty() ? "" : "; first accepted: ", nz.firstUnexpected.c_str());
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("purity-event|%s\n", seen.lines[i].c_str());
	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"noise\":" + Num(noise) + ",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) +
		",\"turns_done\":" + Num(doneTurns) + ",\"stuck\":" + Num(stuck) + ",\"noise_calls\":" + Num(nz.done) + ",\"noise_accepted\":" + Num(nz.ok) + ",\"noise_refused\":" + Num(nz.refused) +
		",\"final_digest\":\"" + Format("%016llx", (unsigned long long)finalDigest) + "\",\"log_errors\":" + Num(seen.logErrors) + ",\"lua_errors\":" + Num(seen.luaErrors) + ",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("purity-summary|noise %d|seed %u|humans %u|ai %u|turns %d/%u|noise calls %lld|final digest %016llx|%s\n", noise, seed, humans, ai, doneTurns, turns, nz.done, (unsigned long long)finalDigest, stopReason.c_str());
	if (inst) gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
