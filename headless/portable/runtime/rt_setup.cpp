// Ported from headless/host/src/game/HstScenario.cpp (ApplyPreGame, G1-G9) and HstJson users; key set of the setup JSON.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <memory>

#include "CvDllContext.h"
#include "CvMinorCivAI.h"
#include "CvPreGame.h"
#include "CvWorldBuilderMapLoader.h"
#include "rt_active.h"
#include "rt_json.h"
#include "rt_session.h"
#include "rt_setup.h"
#include "lekmod_portable/host_log.h"
#include "lekmod_portable/wb_map.h"

namespace lekmod_portable {

namespace {

const int kMaxMajorCivs = 22;   // MAX_MAJOR_CIVS
const int kMaxMinorCivs = 41;   // MAX_MINOR_CIVS
const int kMaxMaxTurns = 10000;  // "max_turns" upper bound (the lobby offers up to 2000)

// SIMPLIFIED(SIMP-018): defaults are the fixed start of B1, a Duel map on the Lekmap Pangaea script; the start screen offers no other
// size or script. The keys stay free so a test or a later build can set any size / script of the DB.
const char* const kDefaultWorldSize = "WORLDSIZE_DUEL";
const char* const kDefaultMapScript = "Assets\\Maps\\Lekmap v5.2\\LekmapPangaeaFractalv5.2.lua";
const char* const kAiHandicap = "HANDICAP_AI_DEFAULT";   // InitPlayers forces AI_HANDICAP for SS_COMPUTER anyway
const char* const kObserverCiv = "CIVILIZATION_AMERICA"; // the observer slot needs a civilization (InitPlayers reads its default color)

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

// splitmix64: integer-only, identical on every target.
struct Rng
{
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed ^ 0x9E3779B97F4A7C15ull) {}
	uint64_t Next()
	{
		uint64_t z = (s += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	size_t Below(size_t n) { return (size_t)(Next() % (uint64_t)n); }
};

// k distinct entries of v (partial Fisher-Yates), in draw order.
template <class T>
std::vector<T> Pick(std::vector<T> v, size_t k, Rng& rng)
{
	for (size_t i = 0; i < k && i < v.size(); ++i)
	{
		const size_t j = i + rng.Below(v.size() - i);
		T t = v[i]; v[i] = v[j]; v[j] = t;
	}
	if (v.size() > k) v.resize(k);
	return v;
}

bool IntOf(const RtJson& v, int& out)
{
	if (v.GetType() == RtJson::JSON_NUMBER) { out = v.AsInt(); return true; }
	if (v.GetType() == RtJson::JSON_BOOL) { out = v.AsBool() ? 1 : 0; return true; }
	return false;
}

bool StringKey(const RtJson& v, const char* key, std::string& out, std::string& err)
{
	if (v.GetType() != RtJson::JSON_STRING) { err = Format("key '%s' must be a string", key); return false; }
	out = v.AsString();
	return true;
}

bool UIntKey(const RtJson& v, const char* key, uint32_t& out, std::string& err)
{
	if (v.GetType() != RtJson::JSON_NUMBER || v.AsNumber() < 0.0 || v.AsNumber() > 4294967295.0)
	{
		err = Format("key '%s' must be a number 0..4294967295", key);
		return false;
	}
	out = (uint32_t)v.AsNumber();
	return true;
}

bool IntKey(const RtJson& v, const char* key, int& out, std::string& err)
{
	if (!IntOf(v, out)) { err = Format("key '%s' must be a number or a boolean", key); return false; }
	return true;
}

bool OptionMap(const RtJson& v, const char* key, std::vector<std::pair<std::string, int> >& out, std::string& err)
{
	if (v.GetType() != RtJson::JSON_OBJECT) { err = Format("key '%s' must be an object", key); return false; }
	out.clear();
	for (size_t i = 0; i < v.Size(); ++i)
	{
		int value = 0;
		if (!IntOf(v.ValueAt(i), value)) { err = Format("key '%s.%s' must be a number or a boolean", key, v.KeyAt(i).c_str()); return false; }
		out.push_back(std::make_pair(v.KeyAt(i), value));
	}
	return true;
}

std::string BackslashPath(std::string s)
{
	for (size_t i = 0; i < s.size(); ++i)
		if (s[i] == '/') s[i] = '\\';
	return s;
}

int TypeId(ICvGameContext1* ctx, const std::string& type, const char* what, std::string& err)
{
	const int id = ctx->GetInfoTypeForString(type.c_str(), true);
	if (id < 0) err = Format("unknown %s '%s'", what, type.c_str());
	return id;
}

// Returns the text column 0 of each row of `sql` (optionally with a text parameter in ?1).
bool Column(Database::Connection& db, const char* sql, const char* param, std::vector<std::string>& out)
{
	Database::Results r;
	if (!db.Execute(r, sql)) return false;
	if (param) r.Bind(1, param);
	while (r.Step())
	{
		const char* t = r.GetText(0);
		if (t) out.push_back(t);
	}
	return true;
}

bool LeaderOf(Database::Connection& db, const std::string& civ, std::string& leader)
{
	std::vector<std::string> v;
	if (!Column(db, "SELECT LeaderheadType FROM Civilization_Leaders WHERE CivilizationType = ? ORDER BY rowid", civ.c_str(), v) || v.empty()) return false;
	leader = v[0];
	return true;
}

// Well-formed UTF-8 without control characters.
bool Utf8Text(const std::string& s)
{
	size_t i = 0;
	while (i < s.size())
	{
		const unsigned char c = (unsigned char)s[i];
		size_t extra = 0;
		unsigned cp = 0;
		if (c < 0x20 || c == 0x7F) return false;
		if (c < 0x80) { ++i; continue; }
		else if (c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; }
		else if (c >= 0xE0 && c <= 0xEF) { extra = 2; cp = c & 0x0F; }
		else if (c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; }
		else return false;
		if (i + extra >= s.size()) return false;
		for (size_t k = 1; k <= extra; ++k)
		{
			const unsigned char d = (unsigned char)s[i + k];
			if ((d & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (d & 0x3F);
		}
		if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
		i += extra + 1;
	}
	return true;
}

// Options the brain sets itself in a network game: a "game_options" entry for them is an error. GAMEOPTION_FIRSTMOVE is
// an error in both game types (decision 3.10).
const char* const kBrainOptions[] = {
	"GAMEOPTION_PITBOSS", "GAMEOPTION_END_TURN_TIMER_ENABLED", "GAMEOPTION_QUICK_COMBAT", "GAMEOPTION_QUICK_MOVEMENT", "GAMEOPTION_FAST_HAND",
	"GAMEOPTION_DYNAMIC_TURNS", "GAMEOPTION_SIMULTANEOUS_TURNS",
};

bool PlayersKey(const RtJson& v, std::vector<RtPlayerSpec>& out, std::string& err)
{
	if (v.GetType() != RtJson::JSON_ARRAY) { err = "key 'players' must be an array"; return false; }
	if (v.Size() == 0 || v.Size() > (size_t)kMaxMajorCivs) { err = Format("key 'players' must hold 1..%d entries", kMaxMajorCivs); return false; }
	out.clear();
	for (size_t i = 0; i < v.Size(); ++i)
	{
		const RtJson& p = v.At(i);
		if (p.GetType() != RtJson::JSON_OBJECT) { err = Format("key 'players[%d]' must be an object", (int)i); return false; }
		RtPlayerSpec spec;
		spec.civ = "random";
		bool kindGiven = false, nickGiven = false;
		for (size_t j = 0; j < p.Size(); ++j)
		{
			const std::string& k = p.KeyAt(j);
			const std::string name = Format("players[%d].%s", (int)i, k.c_str());
			if (k == "kind")
			{
				std::string kind;
				if (!StringKey(p.ValueAt(j), name.c_str(), kind, err)) return false;
				if (kind != "human" && kind != "ai") { err = Format("key '%s' must be \"human\" or \"ai\"", name.c_str()); return false; }
				spec.human = kind == "human";
				kindGiven = true;
			}
			else if (k == "civ") { if (!StringKey(p.ValueAt(j), name.c_str(), spec.civ, err)) return false; }
			else if (k == "nick")
			{
				if (!StringKey(p.ValueAt(j), name.c_str(), spec.nick, err)) return false;
				nickGiven = true;
			}
			else { err = Format("unknown key '%s'", name.c_str()); return false; }
		}
		if (!kindGiven) { err = Format("key 'players[%d].kind' is required", (int)i); return false; }
		if (nickGiven && !spec.human) { err = Format("key 'players[%d].nick' is only for humans", (int)i); return false; }
		if (spec.nick.size() > 64) { err = Format("key 'players[%d].nick' is longer than 64 bytes", (int)i); return false; }
		if (!Utf8Text(spec.nick)) { err = Format("key 'players[%d].nick' is not valid UTF-8 text", (int)i); return false; }
		out.push_back(spec);
	}
	return true;
}

}  // namespace

RtSetup::RtSetup()
	: seed(12345), map_seed(12345), human_civ("CIVILIZATION_ROME"), ai_count(1), world_size(kDefaultWorldSize), map_script(kDefaultMapScript),
	  handicap("HANDICAP_PRINCE"), game_speed("GAMESPEED_STANDARD"), start_era("ERA_ANCIENT"), minors_default(true), minor_count(0), minors_listed(false), max_turns(0),
	  diplo_auto_decline(1), autoplay_human(0), observer(0), game_type("network"), turn_mode("hybrid"), turn_timer("none"), world_builder_scenario(0),
	  world_size_given(false), m_resolved(false)
{
}

int RtSetup::HumanCount() const
{
	int n = 0;
	for (size_t i = 0; i < players.size(); ++i) n += players[i].human ? 1 : 0;
	return n;
}

bool RtSetup::Parse(const std::string& text, std::string& error)
{
	RtSetup fresh;
	RtJson root;
	std::string err;
	if (!RtJson::Parse(text, root, err)) { error = "setup json: " + err; return false; }
	if (root.GetType() != RtJson::JSON_OBJECT) { error = "setup json: root must be an object"; return false; }
	bool mapSeedGiven = false, playersGiven = false, b1FormGiven = false, autoplayHumansGiven = false;
	for (size_t i = 0; i < root.Size(); ++i)
	{
		const std::string& k = root.KeyAt(i);
		const RtJson& v = root.ValueAt(i);
		bool ok = true;
		if (k == "seed") ok = UIntKey(v, "seed", fresh.seed, err);
		else if (k == "map_seed") { ok = UIntKey(v, "map_seed", fresh.map_seed, err); mapSeedGiven = true; }
		else if (k == "players") { ok = PlayersKey(v, fresh.players, err); playersGiven = true; }
		else if (k == "turn_timer") ok = StringKey(v, "turn_timer", fresh.turn_timer, err);
		else if (k == "autoplay_humans")
		{
			if (v.GetType() != RtJson::JSON_ARRAY) { err = "key 'autoplay_humans' must be an array of slot numbers"; ok = false; }
			autoplayHumansGiven = true;
			fresh.autoplay_humans.clear();
			for (size_t j = 0; ok && j < v.Size(); ++j)
			{
				if (v.At(j).GetType() != RtJson::JSON_NUMBER) { err = "key 'autoplay_humans' must be an array of slot numbers"; ok = false; }
				else fresh.autoplay_humans.push_back(v.At(j).AsInt());
			}
		}
		else if (k == "human")
		{
			b1FormGiven = true;
			if (v.GetType() != RtJson::JSON_OBJECT) { err = "key 'human' must be an object"; ok = false; }
			for (size_t j = 0; ok && j < v.Size(); ++j)
			{
				if (v.KeyAt(j) == "civ") ok = StringKey(v.ValueAt(j), "human.civ", fresh.human_civ, err);
				else { err = Format("unknown key 'human.%s'", v.KeyAt(j).c_str()); ok = false; }
			}
		}
		else if (k == "ai_count")
		{
			b1FormGiven = true;
			ok = IntKey(v, "ai_count", fresh.ai_count, err);
			if (ok && (fresh.ai_count < 0 || fresh.ai_count > kMaxMajorCivs - 1)) { err = Format("key 'ai_count' must be 0..%d", kMaxMajorCivs - 1); ok = false; }
		}
		else if (k == "world_size") { ok = StringKey(v, "world_size", fresh.world_size, err); fresh.world_size_given = true; }
		else if (k == "world_builder_map") ok = StringKey(v, "world_builder_map", fresh.world_builder_map, err);
		else if (k == "world_builder_scenario")
		{
			ok = IntKey(v, "world_builder_scenario", fresh.world_builder_scenario, err);
			if (ok && fresh.world_builder_scenario != 0 && fresh.world_builder_scenario != 1) { err = "key 'world_builder_scenario' must be 0 or 1"; ok = false; }
		}
		else if (k == "map_script") { ok = StringKey(v, "map_script", fresh.map_script, err); fresh.map_script = BackslashPath(fresh.map_script); }
		else if (k == "map_options") ok = OptionMap(v, "map_options", fresh.map_options, err);
		else if (k == "handicap") ok = StringKey(v, "handicap", fresh.handicap, err);
		else if (k == "game_speed") ok = StringKey(v, "game_speed", fresh.game_speed, err);
		else if (k == "start_era") ok = StringKey(v, "start_era", fresh.start_era, err);
		else if (k == "minor_civs")
		{
			// "default" | a number | an array of MINOR_CIV_* types
			if (v.GetType() == RtJson::JSON_STRING && v.AsString() == "default") fresh.minors_default = true;
			else if (v.GetType() == RtJson::JSON_NUMBER)
			{
				fresh.minors_default = false;
				fresh.minor_count = v.AsInt();
				if (fresh.minor_count < 0 || fresh.minor_count > kMaxMinorCivs) { err = Format("key 'minor_civs' must be 0..%d", kMaxMinorCivs); ok = false; }
			}
			else if (v.GetType() == RtJson::JSON_ARRAY)
			{
				fresh.minors_default = false;
				fresh.minors_listed = true;
				fresh.minor_list.clear();
				for (size_t j = 0; ok && j < v.Size(); ++j)
				{
					if (v.At(j).GetType() != RtJson::JSON_STRING) { err = "key 'minor_civs' array must hold strings"; ok = false; }
					else fresh.minor_list.push_back(v.At(j).AsString());
				}
				if (ok && (int)fresh.minor_list.size() > kMaxMinorCivs) { err = Format("key 'minor_civs' holds more than %d entries", kMaxMinorCivs); ok = false; }
			}
			else { err = "key 'minor_civs' must be \"default\", a number or an array of types"; ok = false; }
		}
		else if (k == "game_options") ok = OptionMap(v, "game_options", fresh.game_options, err);
		else if (k == "victories") ok = OptionMap(v, "victories", fresh.victories, err);
		else if (k == "max_turns")
		{
			ok = IntKey(v, "max_turns", fresh.max_turns, err);
			if (ok && (fresh.max_turns < 0 || fresh.max_turns > kMaxMaxTurns)) { err = Format("key 'max_turns' must be 0..%d", kMaxMaxTurns); ok = false; }
		}
		else if (k == "auto_choices")
		{
			if (v.GetType() != RtJson::JSON_OBJECT) { err = "key 'auto_choices' must be an object"; ok = false; }
			RtAutoChoices& a = fresh.auto_choices;
			for (size_t j = 0; ok && j < v.Size(); ++j)
			{
				const std::string& n = v.KeyAt(j);
				int* dst = n == "research" ? &a.research : n == "policy" ? &a.policy : n == "ideology" ? &a.ideology : n == "religion" ? &a.religion
					: n == "promotion" ? &a.promotion : n == "great_person" ? &a.great_person : n == "captured_city" ? &a.captured_city
					: n == "league" ? &a.league : n == "archaeology" ? &a.archaeology : n == "goody_hut" ? &a.goody_hut
					: n == "steal_tech" ? &a.steal_tech : NULL;
				if (!dst) { err = Format("unknown key 'auto_choices.%s'", n.c_str()); ok = false; }
				else ok = IntKey(v.ValueAt(j), ("auto_choices." + n).c_str(), *dst, err);
			}
		}
		else if (k == "diplo_auto_decline") ok = IntKey(v, "diplo_auto_decline", fresh.diplo_auto_decline, err);
		else if (k == "autoplay_human") ok = IntKey(v, "autoplay_human", fresh.autoplay_human, err);
		else if (k == "observer") ok = IntKey(v, "observer", fresh.observer, err);
		else if (k == "game_type")
		{
			ok = StringKey(v, "game_type", fresh.game_type, err);
			if (ok && fresh.game_type != "single" && fresh.game_type != "network") { err = "key 'game_type' must be \"single\" or \"network\""; ok = false; }
		}
		else if (k == "turn_mode")
		{
			ok = StringKey(v, "turn_mode", fresh.turn_mode, err);
			if (ok && fresh.turn_mode != "hybrid" && fresh.turn_mode != "simultaneous") { err = "key 'turn_mode' must be \"hybrid\" or \"simultaneous\""; ok = false; }
		}
		else { err = Format("unknown key '%s'", k.c_str()); ok = false; }
		if (!ok) { error = "setup json: " + err; return false; }
	}
	// Slots: "players", or the B1 form (one human in slot 0, then ai_count AI) turned into the same list; both forms together are an error.
	if (playersGiven && b1FormGiven) { error = "setup json: key 'players' excludes the B1 keys 'human' and 'ai_count'"; return false; }
	if (!playersGiven)
	{
		RtPlayerSpec human, ai;
		human.human = true;
		human.civ = fresh.human_civ;
		ai.civ = "random";
		fresh.players.assign(1, human);
		fresh.players.insert(fresh.players.end(), (size_t)fresh.ai_count, ai);
	}
	else
	{
		fresh.ai_count = (int)fresh.players.size() - fresh.HumanCount();
		for (size_t i = 0; i < fresh.players.size(); ++i)
			if (fresh.players[i].human) { fresh.human_civ = fresh.players[i].civ; break; }
	}
	const int humans = fresh.HumanCount();
	if (fresh.Network())
	{
		if (humans < 1 || humans > 8) { error = Format("setup json: key 'players' must hold 1..8 humans in a network game, it holds %d", humans); return false; }
		if ((int)fresh.players.size() > kMaxMajorCivs - 1)
		{
			error = Format("setup json: humans + AI must be at most %d (one major slot is the server seat)", kMaxMajorCivs - 1);
			return false;
		}
	}
	else
	{
		// B1 regression game: one human in slot 0 (or nobody with "observer").
		const bool slot0 = !fresh.players.empty() && fresh.players[0].human;
		if (humans > 1 || (humans == 1 && !slot0)) { error = "setup json: a \"single\" game has at most one human, in slot 0"; return false; }
		if (humans == 0 && !fresh.observer) { error = "setup json: a \"single\" game without a human needs \"observer\": 1"; return false; }
	}
	if (autoplayHumansGiven)
	{
		for (size_t i = 0; i < fresh.autoplay_humans.size(); ++i)
		{
			const int s = fresh.autoplay_humans[i];
			if (s < 0 || s >= (int)fresh.players.size() || !fresh.players[(size_t)s].human)
			{
				error = Format("setup json: key 'autoplay_humans' entry %d is not a human slot", s);
				return false;
			}
		}
		if (!fresh.autoplay_humans.empty()) fresh.autoplay_human = 1;
	}
	if (fresh.turn_timer.empty()) { error = "setup json: key 'turn_timer' must be a TurnTimers type or \"none\""; return false; }
	for (size_t i = 0; i < fresh.game_options.size(); ++i)
	{
		const std::string& name = fresh.game_options[i].first;
		if (name == "GAMEOPTION_FIRSTMOVE")
		{
			error = "setup json: game option 'GAMEOPTION_FIRSTMOVE' is not supported (it relies on the UI path of the first move)";
			return false;
		}
		if (!fresh.Network()) continue;
		for (size_t j = 0; j < sizeof(kBrainOptions) / sizeof(kBrainOptions[0]); ++j)
			if (name == kBrainOptions[j])
			{
				error = Format("setup json: game option '%s' is set by the brain in a network game and cannot be given", name.c_str());
				return false;
			}
	}
	if (fresh.WorldBuilder())
	{
		// the map decides the size (CvWorldBuilderMapLoader::GetCurrentWorldSizeType); isWBMapScript needs the extension.
		const std::string& m = fresh.world_builder_map;
		std::string ext = m.size() >= 8 ? m.substr(m.size() - 8) : std::string();
		for (size_t i = 0; i < ext.size(); ++i) ext[i] = (char)tolower((unsigned char)ext[i]);
		if (ext != ".civ5map") { error = "setup json: key 'world_builder_map' must name a .Civ5Map file"; return false; }
		if (fresh.world_size_given) { error = "setup json: key 'world_size' cannot go with 'world_builder_map' (the map sets the world size)"; return false; }
	}
	else if (fresh.world_builder_scenario) { error = "setup json: key 'world_builder_scenario' needs 'world_builder_map'"; return false; }
	if (!mapSeedGiven) fresh.map_seed = fresh.seed;
	*this = fresh;
	return true;
}

bool RtSetup::Resolve(Database::Connection& db, std::string& error)
{
	civs.clear();
	minors.clear();
	Rng rng(seed);

	civs.assign(players.size(), std::make_pair(std::string(), std::string()));
	std::vector<std::string> taken;   // civilizations in use, explicit and drawn

	// 1. Explicit civilizations of every slot (no random draw); one civilization in two slots is an error.
	for (size_t i = 0; i < players.size(); ++i)
	{
		if (players[i].civ == "random") continue;
		for (size_t j = 0; j < taken.size(); ++j)
			if (taken[j] == players[i].civ) { error = Format("civilization '%s' is used by two slots", players[i].civ.c_str()); return false; }
		std::string leader;
		if (!LeaderOf(db, players[i].civ, leader)) { error = Format("no leader for civilization '%s'", players[i].civ.c_str()); return false; }
		civs[i] = std::make_pair(players[i].civ, leader);
		taken.push_back(players[i].civ);
	}

	// 2. "random" humans, in slot order: one distinct playable civilization each.
	for (size_t i = 0; i < players.size(); ++i)
	{
		if (!players[i].human || players[i].civ != "random") continue;
		std::vector<std::string> playable, rest;
		if (!Column(db, "SELECT c.Type FROM Civilizations c WHERE c.Playable = 1 AND EXISTS (SELECT 1 FROM Civilization_Leaders l WHERE l.CivilizationType = c.Type) ORDER BY c.ID", NULL, playable)
			|| playable.empty())
		{
			error = "no playable civilization in the DB";
			return false;
		}
		for (size_t j = 0; j < playable.size(); ++j)
		{
			bool used = false;
			for (size_t t = 0; t < taken.size(); ++t) used = used || playable[j] == taken[t];
			if (!used) rest.push_back(playable[j]);
		}
		if (rest.empty()) { error = Format("slot %d: no playable civilization left for a random human", (int)i); return false; }
		const std::string civ = rest[rng.Below(rest.size())];
		std::string leader;
		if (!LeaderOf(db, civ, leader)) { error = Format("no leader for civilization '%s'", civ.c_str()); return false; }
		civs[i] = std::make_pair(civ, leader);
		taken.push_back(civ);
	}

	// 3. "random" AI slots: distinct AI civilizations, not one already in use.
	size_t randomAi = 0;
	for (size_t i = 0; i < players.size(); ++i)
		if (!players[i].human && players[i].civ == "random") ++randomAi;
	if (randomAi > 0)
	{
		std::vector<std::string> pool;
		if (!Column(db, "SELECT c.Type FROM Civilizations c WHERE c.AIPlayable = 1 AND EXISTS (SELECT 1 FROM Civilization_Leaders l WHERE l.CivilizationType = c.Type) ORDER BY c.ID", NULL, pool))
		{
			error = "cannot read Civilizations";
			return false;
		}
		std::vector<std::string> candidates;
		for (size_t i = 0; i < pool.size(); ++i)
		{
			bool used = false;
			for (size_t j = 0; j < taken.size(); ++j) used = used || pool[i] == taken[j];
			if (!used) candidates.push_back(pool[i]);
		}
		if (randomAi > candidates.size())
		{
			error = Format("%d random AI slots, only %d AI civilizations available", (int)randomAi, (int)candidates.size());
			return false;
		}
		const std::vector<std::string> chosen = Pick(candidates, randomAi, rng);
		size_t next = 0;
		for (size_t i = 0; i < players.size(); ++i)
		{
			if (players[i].human || players[i].civ != "random") continue;
			std::string leader;
			if (!LeaderOf(db, chosen[next], leader)) { error = Format("no leader for civilization '%s'", chosen[next].c_str()); return false; }
			civs[i] = std::make_pair(chosen[next], leader);
			++next;
		}
	}
	resolved_human_civ = civs.empty() ? std::string() : civs[0].first;
	for (size_t i = 0; i < players.size(); ++i)
		if (players[i].human) { resolved_human_civ = civs[i].first; break; }

	// City-states.
	if (minors_listed) minors = minor_list;
	else
	{
		int count = minor_count;
		if (minors_default)
		{
			Database::Results r;
			count = -1;
			if (db.Execute(r, "SELECT DefaultMinorCivs FROM Worlds WHERE Type = ?"))
			{
				r.Bind(1, world_size.c_str());
				if (r.Step()) count = r.GetInt(0);
			}
			if (count < 0) { error = Format("unknown world size '%s'", world_size.c_str()); return false; }
		}
		std::vector<std::string> all;
		if (!Column(db, "SELECT Type FROM MinorCivilizations ORDER BY ID", NULL, all)) { error = "cannot read MinorCivilizations"; return false; }
		if (count > kMaxMinorCivs || (size_t)count > all.size())
		{
			error = Format("minor_civs %d, only %d city-states available", count, (int)all.size());
			return false;
		}
		minors = Pick(all, (size_t)count, rng);
	}
	m_resolved = true;
	return true;
}

bool RtSetup::Validate(RtSession& session, std::string& error) const
{
	if (!session.Loaded() || !session.Db().Connection()) { error = "session is not loaded"; return false; }
	Database::Connection& db = *session.Db().Connection();
	struct Check { const char* key; const std::string* value; const char* sql; };
	const Check checks[] = {
		{"world_size", &world_size, "SELECT 1 FROM Worlds WHERE Type = ?"},
		{"start_era", &start_era, "SELECT 1 FROM Eras WHERE Type = ?"},
		{"game_speed", &game_speed, "SELECT 1 FROM GameSpeeds WHERE Type = ?"},
		{"handicap", &handicap, "SELECT 1 FROM HandicapInfos WHERE Type = ?"},
	};
	for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i)
	{
		Database::Results r;
		bool known = false;
		if (db.Execute(r, checks[i].sql))
		{
			r.Bind(1, checks[i].value->c_str());
			known = r.Step();
		}
		if (!known) { error = Format("%s '%s' is unknown", checks[i].key, checks[i].value->c_str()); return false; }
	}
	for (size_t i = 0; i < players.size(); ++i)
	{
		if (players[i].civ == "random") continue;
		Database::Results r;
		bool known = false;
		if (db.Execute(r, "SELECT 1 FROM Civilizations WHERE Type = ?"))
		{
			r.Bind(1, players[i].civ.c_str());
			known = r.Step();
		}
		if (!known) { error = Format("players[%d].civ '%s' is unknown", (int)i, players[i].civ.c_str()); return false; }
	}
	if (turn_timer != "none")
	{
		Database::Results r;
		bool known = false;
		if (db.Execute(r, "SELECT 1 FROM TurnTimers WHERE Type = ?"))
		{
			r.Bind(1, turn_timer.c_str());
			known = r.Step();
		}
		if (!known) { error = Format("turn_timer '%s' is unknown", turn_timer.c_str()); return false; }
	}
	for (size_t i = 0; minors_listed && i < minor_list.size(); ++i)
	{
		Database::Results r;
		bool known = false;
		if (db.Execute(r, "SELECT 1 FROM MinorCivilizations WHERE Type = ?"))
		{
			r.Bind(1, minor_list[i].c_str());
			known = r.Step();
		}
		if (!known) { error = Format("minor_civs '%s' is unknown", minor_list[i].c_str()); return false; }
	}
	for (size_t i = 0; i < victories.size(); ++i)
	{
		Database::Results r;
		bool known = false;
		if (db.Execute(r, "SELECT 1 FROM Victories WHERE Type = ?"))
		{
			r.Bind(1, victories[i].first.c_str());
			known = r.Step();
		}
		if (!known) { error = Format("unknown victory '%s'", victories[i].first.c_str()); return false; }
	}
	// A WorldBuilder map instead of the map script: it must be readable through the host.
	if (WorldBuilder())
	{
		std::string bytes;
		const std::wstring wide(world_builder_map.begin(), world_builder_map.end());
		if (!ReadWbMapFile(wide.c_str(), bytes) || bytes.size() < 10) { error = Format("world_builder_map '%s' is not readable", world_builder_map.c_str()); return false; }
		if (world_builder_scenario && !((unsigned char)bytes[0] & 0x80))
		{
			error = Format("world_builder_map '%s' has no scenario data (world_builder_scenario 1)", world_builder_map.c_str());
			return false;
		}
	}
	// Map script: a file of the game data (compared case-insensitively, '/' = backslash, by suffix) or a script with MapScriptOptions rows.
	else
	{
		Database::Results r;
		bool known = false;
		if (db.Execute(r, "SELECT 1 FROM MapScriptOptions WHERE FileName = ?"))
		{
			r.Bind(1, map_script.c_str());
			known = r.Step();
		}
		if (!known)
		{
			std::string want = map_script;
			for (size_t i = 0; i < want.size(); ++i) want[i] = want[i] == '\\' ? '/' : (char)tolower((unsigned char)want[i]);
			const std::vector<std::string>& files = session.Vfs().Files();
			for (size_t i = 0; i < files.size() && !known; ++i)
			{
				std::string f = files[i];
				for (size_t j = 0; j < f.size(); ++j) f[j] = f[j] == '\\' ? '/' : (char)tolower((unsigned char)f[j]);
				known = f == want || (f.size() > want.size() && f.compare(f.size() - want.size(), want.size(), want) == 0) ||
					(want.size() > f.size() && want.compare(want.size() - f.size(), f.size(), f) == 0);
			}
		}
		if (!known) { error = Format("map_script '%s' is unknown", map_script.c_str()); return false; }
	}
	return true;
}

void RtSetup::ApplyVictoriesAndTurns() const
{
	// PreGame setVictory per Victories row (resetGame made them all valid); the names were checked by Validate / Apply. max_turns 0 keeps whatever
	// is set (the default 0, or a scenario's own).
	for (size_t i = 0; i < victories.size(); ++i)
	{
		const int id = GC.getInfoTypeForString(victories[i].first.c_str(), true);
		if (id >= 0) CvPreGame::setVictory((VictoryTypes)id, victories[i].second != 0);
	}
	if (max_turns > 0) CvPreGame::setMaxTurns(max_turns);
}

void RtSetup::ForceGameOptions() const
{
	// The setup's "game_options", then the options the brain sets itself (as G6 and SIMP-010 in Apply), over what a scenario set.
	for (size_t i = 0; i < game_options.size(); ++i)
		CvPreGame::SetGameOption(game_options[i].first.c_str(), game_options[i].second);
	ApplyVictoriesAndTurns();
	if (Network())
	{
		const bool hybrid = turn_mode == "hybrid";
		CvPreGame::SetGameOption("GAMEOPTION_PITBOSS", 1);
		CvPreGame::SetGameOption("GAMEOPTION_END_TURN_TIMER_ENABLED", 0);
		CvPreGame::SetGameOption("GAMEOPTION_DYNAMIC_TURNS", hybrid ? 1 : 0);
		CvPreGame::SetGameOption("GAMEOPTION_SIMULTANEOUS_TURNS", hybrid ? 0 : 1);
	}
	CvPreGame::setQuickCombat(true);
	CvPreGame::setQuickMovement(true);
}

bool RtSetup::SetupScenarioPlayers(RtSession& session, std::string& error)
{
	CvDllGameContext* ctx = session.Context();
	Database::Connection& db = *session.Db().Connection();
	std::string err;
	CvWorldBuilderMapLoader::SetupPlayers();
	const int aiHandicap = TypeId(ctx, kAiHandicap, "handicap", err);
	if (aiHandicap < 0) { error = err; return false; }
	for (size_t i = 0; i < players.size(); ++i)
	{
		const PlayerTypes slot = (PlayerTypes)i;
		const CivilizationTypes civ = CvPreGame::civilization(slot);
		const CvCivilizationInfo* info = civ != NO_CIVILIZATION ? GC.getCivilizationInfo(civ) : NULL;
		if (!info) { error = Format("world_builder_map: scenario player %d has a civilization the game data does not know", (int)i); return false; }
		const std::string type = info->GetType();
		if (players[i].civ != "random" && players[i].civ != type)
		{
			error = Format("players[%d].civ '%s' differs from the scenario's '%s'", (int)i, players[i].civ.c_str(), type.c_str());
			return false;
		}
		std::string leader;
		if (!LeaderOf(db, type, leader)) { error = Format("no leader for civilization '%s'", type.c_str()); return false; }
		const int l = TypeId(ctx, leader, "leader", err);
		if (l < 0) { error = err; return false; }
		CvPreGame::setLeaderHead(slot, (LeaderHeadTypes)l);
		// An AI handicap the game data does not know (the map names it) falls back to the AI default.
		if (CvPreGame::handicap(slot) == NO_HANDICAP) CvPreGame::setHandicap(slot, (HandicapTypes)aiHandicap);
		civs[i] = std::make_pair(type, leader);
	}
	// City-states: the scenario's, or as many of the chosen ones as the map has minor start positions; an unknown one ends the list.
	minors.clear();
	int count = CvPreGame::numMinorCivs();
	for (int j = 0; j < count; ++j)
	{
		const MinorCivTypes m = CvPreGame::minorCivType((PlayerTypes)(kMaxMajorCivs + j));
		const CvMinorCivInfo* info = m != NO_MINORCIV ? GC.getMinorCivInfo(m) : NULL;
		if (!info)
		{
			Log(LOG_WARN, "setup: world_builder_map minor slot %d has no known city-state, %d city-state(s) kept", j, j);
			CvPreGame::setNumMinorCivs(j);
			break;
		}
		minors.push_back(info->GetType());
	}
	return true;
}

void RtSetup::AfterGameInit() const
{
	if (!WorldBuilder() || !world_builder_scenario) return;
	// CvGame::init set the start year to START_YEAR; SetupGameOptions sets the scenario's again (and the options, which are unchanged).
	const GameSpeedTypes speed = CvPreGame::gameSpeed();
	const int maxTurns = CvPreGame::maxTurns();
	CvWorldBuilderMapLoader::SetupGameOptions();
	CvPreGame::setGameSpeed(speed);
	CvPreGame::setMaxTurns(maxTurns);
	ForceGameOptions();
}

bool RtSetup::Apply(RtSession& session, std::string& error)
{
	if (!session.Loaded() || !session.Context()) { error = "session is not loaded"; return false; }
	CvDllGameContext* ctx = session.Context();
	std::string err;
	// P14: a WorldBuilder map is preloaded first (the exe preloads the map the staging room picks); it gives the world size, which Resolve
	// needs for the default city-state count.
	if (WorldBuilder())
	{
		CvWorldBuilderMapLoader::ResetPlayerSlots();   // sg_auiPlayerSlots starts zeroed: Preload would swap the players otherwise
		const std::wstring wide(world_builder_map.begin(), world_builder_map.end());
		if (!CvWorldBuilderMapLoader::Preload(wide.c_str(), world_builder_scenario != 0))
		{
			error = Format("world_builder_map '%s' cannot be loaded", world_builder_map.c_str());
			return false;
		}
		const int size = (int)CvWorldBuilderMapLoader::GetCurrentWorldSizeType();
		std::vector<std::string> sizeType;
		const std::string sizeId = Format("%d", size);
		if (size < 0 || !Column(*session.Db().Connection(), "SELECT Type FROM Worlds WHERE ID = ?", sizeId.c_str(), sizeType) || sizeType.empty())
		{
			error = Format("world_builder_map '%s': no world size fits the map", world_builder_map.c_str());
			return false;
		}
		world_size = sizeType[0];
		const CvWorldBuilderMapLoaderMapInfo& map = CvWorldBuilderMapLoader::GetCurrentMapInfo();
		if (world_builder_scenario && (map.uiPlayers == 0 || map.uiPlayers != players.size()))
		{
			error = Format("world_builder_map '%s' has %u scenario players, key 'players' holds %d", world_builder_map.c_str(), map.uiPlayers,
				(int)players.size());
			return false;
		}
	}
	if (!m_resolved && !Resolve(*session.Db().Connection(), error)) return false;
	std::auto_ptr<ICvPreGame1> pre(ctx->GetPreGame());

	// Resolve every type up front so a bad name fails before the pregame is touched.
	const int world = TypeId(ctx, world_size, "world size", err);
	const int era = world < 0 ? -1 : TypeId(ctx, start_era, "era", err);
	const int speed = era < 0 ? -1 : TypeId(ctx, game_speed, "game speed", err);
	const int humanHandicap = speed < 0 ? -1 : TypeId(ctx, handicap, "handicap", err);
	const int aiHandicap = humanHandicap < 0 ? -1 : TypeId(ctx, kAiHandicap, "handicap", err);
	const int climate = aiHandicap < 0 ? -1 : TypeId(ctx, "CLIMATE_TEMPERATE", "climate", err);
	const int seaLevel = climate < 0 ? -1 : TypeId(ctx, "SEALEVEL_MEDIUM", "sea level", err);
	if (seaLevel < 0) { error = err; return false; }
	std::vector<int> civIds, leaderIds, minorIds;
	for (size_t i = 0; i < civs.size(); ++i)
	{
		const int c = TypeId(ctx, civs[i].first, "civilization", err);
		const int l = c < 0 ? -1 : TypeId(ctx, civs[i].second, "leader", err);
		if (l < 0) { error = err; return false; }
		civIds.push_back(c);
		leaderIds.push_back(l);
	}
	for (size_t i = 0; i < minors.size(); ++i)
	{
		const int m = TypeId(ctx, minors[i], "minor civ", err);
		if (m < 0) { error = err; return false; }
		minorIds.push_back(m);
	}
	// CvPreGame::SetGameOption accepts any name (it appends), so the names are checked against the GameOptions table here.
	for (size_t i = 0; i < game_options.size(); ++i)
	{
		Database::Results r;
		bool known = false;
		if (session.Db().Connection()->Execute(r, "SELECT 1 FROM GameOptions WHERE Type = ?"))
		{
			r.Bind(1, game_options[i].first.c_str());
			known = r.Step();
		}
		if (!known) { error = Format("unknown game option '%s'", game_options[i].first.c_str()); return false; }
	}
	for (size_t i = 0; i < victories.size(); ++i)
		if (TypeId(ctx, victories[i].first, "victory", err) < 0) { error = err; return false; }
	int observerCiv = -1, observerLeader = -1;
	// a network game always has the server seat (an SS_OBSERVER slot) as its active player.
	const bool seat = observer || Network();
	if (seat)
	{
		if ((int)civs.size() >= kMaxMajorCivs) { error = "no free major slot for the observer"; return false; }
		observerCiv = TypeId(ctx, kObserverCiv, "observer civilization", err);
		if (observerCiv < 0) { error = err; return false; }
		// civhost leaves the observer without a leader; CvPlayerTechs::Reset takes &getLeaderInfo() and tests it for NULL, which clang folds away
		// (reference), so a NO_LEADER observer crashes in the portable build. A leader of the observer civilization avoids that.
		std::string leader;
		if (!LeaderOf(*session.Db().Connection(), kObserverCiv, leader)) { error = "observer civilization has no leader"; return false; }
		observerLeader = TypeId(ctx, leader, "observer leader", err);
		if (observerLeader < 0) { error = err; return false; }
	}

	// G1
	pre->resetSlots();
	pre->resetGame();
	pre->resetPlayers();
	pre->setSyncRandomSeed(seed);
	pre->setMapRandomSeed(map_seed);

	// G2. Network (spike S1): only GAME_NETWORK_MULTIPLAYER keeps simultaneous / hybrid turns and Pitboss on
	// (CvGame.cpp:227-247).
	pre->setGameType(Network() ? GAME_NETWORK_MULTIPLAYER : GAME_SINGLE_PLAYER);

	// G3: with bResetSlots it sets 8 default SS_COMPUTER slots, so it goes before the slot setup.
	pre->setWorldSize((WorldSizeTypes)world, true);

	// G4: must equal MapScriptOptions.FileName (GetMapOption falls back to that table). A .Civ5Map name makes CvPreGame::isWBMapScript true:
	// CvGame::InitMap then builds the map with CvWorldBuilderMapLoader instead of the script.
	pre->setMapScriptName(CvString(WorldBuilder() ? world_builder_map : map_script));

	// G5: every option of the script explicitly (defaults from MapScriptOptions, then the overrides). A WorldBuilder map has none.
	applied_map_options.clear();
	if (!WorldBuilder())
	{
		Database::Results r;
		if (session.Db().Connection()->Execute(r, "SELECT OptionID, DefaultValue FROM MapScriptOptions WHERE FileName = ? ORDER BY rowid"))
		{
			r.Bind(1, map_script.c_str());
			while (r.Step())
			{
				const char* id = r.GetText(0);
				if (id) applied_map_options.push_back(std::make_pair(std::string(id), r.GetInt(1)));
			}
		}
		for (size_t i = 0; i < map_options.size(); ++i)
		{
			bool found = false;
			for (size_t j = 0; j < applied_map_options.size(); ++j)
				if (applied_map_options[j].first == map_options[i].first) { applied_map_options[j].second = map_options[i].second; found = true; }
			if (!found) applied_map_options.push_back(map_options[i]);
		}
		if (applied_map_options.empty()) Log(LOG_WARN, "setup: no MapScriptOptions rows for '%s'", map_script.c_str());
		for (size_t i = 0; i < applied_map_options.size(); ++i)
			pre->SetMapOption(applied_map_options[i].first.c_str(), applied_map_options[i].second);
	}

	// G6
	pre->setClimate((ClimateTypes)climate);
	pre->setSeaLevel((SeaLevelTypes)seaLevel);
	pre->setEra((EraTypes)era);
	pre->setGameSpeed((GameSpeedTypes)speed);
	for (size_t i = 0; i < game_options.size(); ++i)
		pre->SetGameOption(game_options[i].first.c_str(), game_options[i].second);
	if (Network())
	{
		// Pitboss (no player of the server's own), no turn timer in the brain (decision 3.8), the turn mode as the
		// staging room sets it (MPGameOptions.lua OnTurnModePull: hybrid = DYNAMIC_TURNS alone, simultaneous = SIMULTANEOUS_TURNS alone).
		pre->SetGameOption("GAMEOPTION_PITBOSS", 1);
		pre->SetGameOption("GAMEOPTION_END_TURN_TIMER_ENABLED", 0);
		const bool hybrid = turn_mode == "hybrid";
		pre->SetGameOption("GAMEOPTION_DYNAMIC_TURNS", hybrid ? 1 : 0);
		pre->SetGameOption("GAMEOPTION_SIMULTANEOUS_TURNS", hybrid ? 0 : 1);
		// The TurnTimers row (setGameSpeed picked the speed's own one): the brain only reads it, for Q_GAME.turn_len_s / turn_base_s; the timer itself is the server's (3.8).
		if (turn_timer != "none") pre->setTurnTimer(CvString(turn_timer));
	}
	// Victories and the turn limit of the staging room (all victories are valid after resetGame; max_turns 0 = the speed's default).
	pre->setMaxTurns(0);
	ApplyVictoriesAndTurns();
	if (WorldBuilder())
	{
		// P14: the scenario's own options, victories, max turns, start year and speed (CvWorldBuilderMapLoader::SetupGameOptions); then the
		// setup's "game_options" and the options the brain sets itself win again. The humans keep the setup's handicap.
		CvPreGame::setLoadWBScenario(world_builder_scenario != 0);
		CvPreGame::setOverrideScenarioHandicap(true);
		CvPreGame::setMapNoPlayers(world_builder_scenario == 0);
		if (world_builder_scenario)
		{
			CvWorldBuilderMapLoader::SetupGameOptions();
			// A speed name the game data does not know leaves no speed: the setup's then.
			if (CvPreGame::gameSpeed() < 0 || !GC.getGameSpeedInfo(CvPreGame::gameSpeed())) pre->setGameSpeed((GameSpeedTypes)speed);
			ForceGameOptions();
		}
	}
	// SIMPLIFIED(SIMP-010): units have no animations in B1, so quick combat and quick movement are forced on (in the original the
	// player chooses); the "game_options" of the same names are overridden.
	pre->setQuickCombat(true);
	pre->setQuickMovement(true);

	// G7: the "players" in slot order, humans SS_TAKEN (with their nickname in a network game) and AI SS_COMPUTER; the server
	// seat follows; the rest is closed (never SS_OPEN: in Pitboss the core counts those as humans, CvPreGame.cpp:1296).
	for (int i = 0; i < kMaxMajorCivs; ++i)
	{
		const PlayerTypes slot = (PlayerTypes)i;
		if (i < (int)civs.size())
		{
			// observer mode (B1): every civilization is an AI, nobody is human
			const bool human = players[(size_t)i].human && !observer;
			pre->setSlotStatus(slot, human ? SS_TAKEN : SS_COMPUTER);
			if (human && Network())
				CvPreGame::setNickname(slot, CvString(players[(size_t)i].nick.empty() ? Format("human%d", i) : players[(size_t)i].nick));
			pre->setSlotClaim(slot, SLOTCLAIM_ASSIGNED);
			pre->setCivilization(slot, (CivilizationTypes)civIds[i]);
			pre->setLeaderHead(slot, (LeaderHeadTypes)leaderIds[i]);
			// SIMPLIFIED(SIMP-054): every player (human and AI) gets a team of its own; the original setup groups players into teams.
			pre->setTeamType(slot, (TeamTypes)i);
			pre->setHandicap(slot, (HandicapTypes)(human ? humanHandicap : aiHandicap));
		}
		else
			pre->setSlotStatus(slot, SS_CLOSED);
	}

	// G8: mandatory, InitPlayers dereferences GC.getMinorCivInfo(minorCivType(p)).
	pre->setNumMinorCivs((int)minorIds.size());
	for (size_t j = 0; j < minorIds.size(); ++j)
		pre->setMinorCivType((PlayerTypes)(kMaxMajorCivs + (int)j), (MinorCivTypes)minorIds[j]);

	// P14: the scenario's players take the slots (civilization, color, team, the AI's handicap) and its city-states the minor slots
	// (CvWorldBuilderMapLoader::SetupPlayers); each slot gets the first leader of its civilization.
	if (WorldBuilder() && world_builder_scenario && !SetupScenarioPlayers(session, err)) { error = err; return false; }

	// Observer: an SS_OBSERVER major slot right after the civs (as civhost R9). InitPlayers moves it to OBSERVER_TEAM.
	int active = 0;
	if (seat)
	{
		const PlayerTypes slot = (PlayerTypes)civs.size();
		pre->setSlotStatus(slot, SS_OBSERVER);
		pre->setSlotClaim(slot, SLOTCLAIM_ASSIGNED);
		pre->setCivilization(slot, (CivilizationTypes)observerCiv);
		pre->setLeaderHead(slot, (LeaderHeadTypes)observerLeader);
		pre->setTeamType(slot, (TeamTypes)slot);
		pre->setHandicap(slot, (HandicapTypes)aiHandicap);
		active = (int)slot;
	}

	// G9
	pre->setActivePlayer((PlayerTypes)active);
	SetServerSeat(Network() ? (PlayerTypes)active : NO_PLAYER);
	return true;
}

std::string RtSetup::Describe() const
{
	std::string s = Format("setup: seed=%u map_seed=%u %s %s %s %s %s ai=%d observer=%d", (unsigned)seed, (unsigned)map_seed, world_size.c_str(),
		map_script.c_str(), handicap.c_str(), game_speed.c_str(), start_era.c_str(), ai_count, observer);
	if (Network()) s += Format(" game_type=network humans=%d turn_mode=%s turn_timer=%s", HumanCount(), turn_mode.c_str(), turn_timer.c_str());
	if (WorldBuilder()) s += Format(" world_builder_map=%s scenario=%d", world_builder_map.c_str(), world_builder_scenario);
	s += "\n  civs:";
	for (size_t i = 0; i < civs.size(); ++i)
		s += " " + civs[i].first + "/" + civs[i].second + (i < players.size() && players[i].human ? "(human)" : "");
	s += "\n  minors:";
	for (size_t i = 0; i < minors.size(); ++i) s += " " + minors[i];
	return s;
}

}  // namespace lekmod_portable
