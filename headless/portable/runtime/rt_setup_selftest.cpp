// Scenario `setup` (run.sh --scenario setup): rt_setup. Part 1 needs no game: parse of a JSON with every key of ABI 4.2, defaults, unknown /
// mistyped keys. Part 2 (needs --game): RtSession::Load, deterministic AI / city-state choice from the seed, Apply (G1-G9) and the
// PreGame values read back through ICvPreGame1. The game is not started (no Init / Init2).
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <set>
#include <string>

#include "CvDllContext.h"
#include "rt_active.h"
#include "rt_selftest.h"
#include "rt_session.h"
#include "rt_setup.h"
#include "lekmod_portable/host_log.h"

using namespace lekmod_portable;

namespace {

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		std::string esc;
		for (const char* p = detail; *p; ++p)
		{
			if (*p == '"' || *p == '\\') esc += '\\';
			if ((unsigned char)*p >= 0x20) esc += *p;
		}
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + esc + "\"}";
		printf("setup: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

// The v2 form: three humans, two AI, every v2 key.
const char kPlayersJson[] =
	"{\"game_type\":\"network\",\"seed\":99,\"players\":[{\"kind\":\"human\",\"civ\":\"CIVILIZATION_ROME\",\"nick\":\"player1\"},"
	"{\"kind\":\"human\",\"civ\":\"random\",\"nick\":\"\xD0\xB4\xD1\x80\xD1\x83\xD0\xB3\"},{\"kind\":\"ai\",\"civ\":\"CIVILIZATION_EGYPT\"},"
	"{\"kind\":\"ai\",\"civ\":\"random\"},{\"kind\":\"human\",\"civ\":\"random\"},{\"kind\":\"ai\"}],"
	"\"turn_mode\":\"simultaneous\",\"turn_timer\":\"none\",\"world_size\":\"WORLDSIZE_TINY\",\"handicap\":\"HANDICAP_EMPEROR\",\"autoplay_humans\":[1,4]}";

// Every key of ABI 4.2, values different from the defaults.
const char kFullJson[] =
	"{\"game_type\":\"single\",\"seed\":777,\"map_seed\":4242,\"human\":{\"civ\":\"CIVILIZATION_ROME\"},\"ai_count\":2,\"world_size\":\"WORLDSIZE_TINY\","
	"\"map_script\":\"Assets/Maps/Lekmap v5.2/LekmapPangaeaFractalv5.2.lua\",\"map_options\":{\"9\":3},"
	"\"handicap\":\"HANDICAP_EMPEROR\",\"game_speed\":\"GAMESPEED_QUICK\",\"start_era\":\"ERA_CLASSICAL\",\"minor_civs\":3,"
	"\"game_options\":{\"GAMEOPTION_NO_GOODY_HUTS\":1,\"GAMEOPTION_QUICK_COMBAT\":0},"
	"\"auto_choices\":{\"research\":0,\"policy\":0,\"ideology\":0,\"religion\":0,\"promotion\":0,\"great_person\":0,\"captured_city\":0,"
	"\"league\":0,\"archaeology\":0,\"goody_hut\":0,\"steal_tech\":0},\"diplo_auto_decline\":0,\"autoplay_human\":1,\"observer\":1}";

const char kLekmapPangaea[] = "Assets\\Maps\\Lekmap v5.2\\LekmapPangaeaFractalv5.2.lua";

bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void RunParse(Check& c)
{
	std::string err;
	RtSetup full;
	const bool parsed = full.Parse(kFullJson, err);
	c.add("parse_full", parsed, "%s", err.c_str());
	if (parsed)
	{
		const RtAutoChoices& a = full.auto_choices;
		c.add("parse_full_values",
			full.seed == 777 && full.map_seed == 4242 && full.human_civ == "CIVILIZATION_ROME" && full.ai_count == 2 && full.world_size == "WORLDSIZE_TINY"
				&& full.map_script == kLekmapPangaea && full.map_options.size() == 1 && full.map_options[0].first == "9" && full.map_options[0].second == 3
				&& full.handicap == "HANDICAP_EMPEROR" && full.game_speed == "GAMESPEED_QUICK" && full.start_era == "ERA_CLASSICAL" && !full.minors_default
				&& full.minor_count == 3 && full.game_options.size() == 2 && full.game_options[0].first == "GAMEOPTION_NO_GOODY_HUTS"
				&& full.game_options[1].second == 0 && full.diplo_auto_decline == 0 && full.autoplay_human == 1 && full.observer == 1,
			"seed=%u map_seed=%u ai=%d size=%s speed=%s era=%s handicap=%s minors=%d observer=%d", (unsigned)full.seed, (unsigned)full.map_seed, full.ai_count,
			full.world_size.c_str(), full.game_speed.c_str(), full.start_era.c_str(), full.handicap.c_str(), full.minor_count, full.observer);
		c.add("parse_auto_choices", a.research == 0 && a.policy == 0 && a.ideology == 0 && a.religion == 0 && a.promotion == 0 && a.great_person == 0
			&& a.captured_city == 0 && a.league == 0 && a.archaeology == 0 && a.goody_hut == 0 && a.steal_tech == 0, "all 11 auto_choices flags read as 0");
	}

	RtSetup def;
	c.add("parse_defaults", def.Parse("{}", err) && def.seed == 12345 && def.map_seed == 12345 && def.human_civ == "CIVILIZATION_ROME" && def.ai_count == 1
		&& def.world_size == "WORLDSIZE_DUEL" && def.map_script == kLekmapPangaea && def.handicap == "HANDICAP_PRINCE" && def.game_speed == "GAMESPEED_STANDARD"
		&& def.start_era == "ERA_ANCIENT" && def.minors_default && def.auto_choices.research == 1 && def.auto_choices.steal_tech == 1
		&& def.diplo_auto_decline == 1 && def.autoplay_human == 0 && def.observer == 0, "{} gives the ABI 4.2 example values");
	RtSetup seedOnly;
	c.add("parse_map_seed_default", seedOnly.Parse("{\"seed\":99}", err) && seedOnly.map_seed == 99, "map_seed follows seed when absent");
	RtSetup listed;
	c.add("parse_minor_forms", listed.Parse("{\"minor_civs\":[\"MINOR_CIV_ALMATY\",\"MINOR_CIV_ANTWERP\"]}", err) && listed.minors_listed && listed.minor_list.size() == 2
		&& listed.minor_list[1] == "MINOR_CIV_ANTWERP", "minor_civs as an array");
	// Lobby options (login-lobby P2): victories and max_turns.
	RtSetup vic;
	c.add("parse_victories_max_turns", vic.Parse("{\"victories\":{\"VICTORY_TIME\":0,\"VICTORY_CULTURAL\":1},\"max_turns\":300}", err) && vic.victories.size() == 2
		&& vic.victories[0].first == "VICTORY_TIME" && vic.victories[0].second == 0 && vic.victories[1].second == 1 && vic.max_turns == 300, "%s", err.c_str());
	c.add("parse_victories_defaults", def.victories.empty() && def.max_turns == 0, "no victories listed and max_turns 0 by default");
	// P14 keys.
	RtSetup wb, wbSize, wbExt, wbNoMap;
	c.add("parse_world_builder", wb.Parse("{\"world_builder_map\":\"maps/Tutorials/Maps/LateGameTest.Civ5Map\",\"world_builder_scenario\":1}", err)
		&& wb.WorldBuilder() && wb.world_builder_scenario == 1, "%s", err.c_str());
	c.add("parse_world_builder_size_error", !wbSize.Parse("{\"world_builder_map\":\"a.Civ5Map\",\"world_size\":\"WORLDSIZE_DUEL\"}", err) && Contains(err, "world_size"),
		"%s", err.c_str());
	c.add("parse_world_builder_ext_error", !wbExt.Parse("{\"world_builder_map\":\"a.lua\"}", err) && Contains(err, ".Civ5Map"), "%s", err.c_str());
	c.add("parse_world_builder_scenario_error", !wbNoMap.Parse("{\"world_builder_scenario\":1}", err) && Contains(err, "world_builder_map"), "%s", err.c_str());

	// v2 defaults and the players form.
	c.add("parse_v2_defaults", def.Network() && def.turn_mode == "hybrid" && def.turn_timer == "none" && def.players.size() == 2 && def.players[0].human && !def.players[1].human
		&& def.HumanCount() == 1 && def.autoplay_humans.empty(), "{} is a network game, hybrid, no timer, B1 form = 1 human + 1 AI");
	RtSetup pl;
	const bool plOk = pl.Parse(kPlayersJson, err);
	c.add("parse_players", plOk, "%s", err.c_str());
	if (plOk)
		c.add("parse_players_values", pl.players.size() == 6 && pl.HumanCount() == 3 && pl.ai_count == 3 && pl.players[0].nick == "player1" && pl.players[0].civ == "CIVILIZATION_ROME"
			&& pl.players[1].nick == "\xD0\xB4\xD1\x80\xD1\x83\xD0\xB3" && pl.players[2].civ == "CIVILIZATION_EGYPT" && !pl.players[2].human && pl.players[5].civ == "random"
			&& pl.turn_mode == "simultaneous" && pl.autoplay_humans.size() == 2 && pl.autoplay_humans[1] == 4 && pl.autoplay_human == 1 && pl.human_civ == "CIVILIZATION_ROME",
			"6 slots, 3 humans, nicks, civs, turn_mode %s, autoplay_humans %d", pl.turn_mode.c_str(), (int)pl.autoplay_humans.size());
	RtSetup single;
	c.add("parse_single_players", single.Parse("{\"game_type\":\"single\",\"players\":[{\"kind\":\"human\",\"civ\":\"CIVILIZATION_ROME\"},{\"kind\":\"ai\"}]}", err) && !single.Network()
		&& single.players.size() == 2, "single game with the players form");
	std::string maxNick(64, 'n');
	RtSetup nick64;
	c.add("parse_nick_64_bytes", nick64.Parse("{\"players\":[{\"kind\":\"human\",\"nick\":\"" + maxNick + "\"}]}", err), "a 64-byte nick is accepted: %s", err.c_str());
	RtSetup eight;
	{
		std::string j = "{\"players\":[";
		for (int i = 0; i < 8; ++i) j += std::string(i ? "," : "") + "{\"kind\":\"human\"}";
		for (int i = 0; i < 13; ++i) j += ",{\"kind\":\"ai\"}";
		j += "]}";
		c.add("parse_8_humans_21_slots", eight.Parse(j, err) && eight.players.size() == 21, "8 humans + 13 AI = 21 slots: %s", err.c_str());
	}

	const struct { const char* json; const char* mustName; } bad[] = {
		{"{\"seed\":1,\"bogus_key\":2}", "bogus_key"},
		{"{\"players\":[{\"kind\":\"human\"}],\"ai_count\":2}", "excludes"},
		{"{\"players\":[{\"kind\":\"human\"}],\"human\":{\"civ\":\"CIVILIZATION_ROME\"}}", "excludes"},
		{"{\"players\":[{\"kind\":\"human\",\"colour\":1}]}", "players[0].colour"},
		{"{\"players\":[{\"civ\":\"CIVILIZATION_ROME\"}]}", "players[0].kind"},
		{"{\"players\":[{\"kind\":\"robot\"}]}", "players[0].kind"},
		{"{\"players\":[{\"kind\":\"human\"},{\"kind\":\"ai\",\"nick\":\"x\"}]}", "players[1].nick"},
		{"{\"players\":[{\"kind\":\"human\",\"nick\":\"123456789012345678901234567890123456789012345678901234567890-----\"}]}", "64 bytes"},
		{"{\"players\":[{\"kind\":\"human\",\"nick\":\"\xFF\xFE\"}]}", "UTF-8"},
		{"{\"players\":[{\"kind\":\"human\",\"civ\":5}]}", "players[0].civ"},
		{"{\"players\":[]}", "players"},
		{"{\"players\":{}}", "players"},
		{"{\"players\":[{\"kind\":\"ai\"},{\"kind\":\"ai\"}]}", "humans"},
		{"{\"players\":[{\"kind\":\"human\"},{\"kind\":\"human\"},{\"kind\":\"human\"},{\"kind\":\"human\"},{\"kind\":\"human\"},{\"kind\":\"human\"},{\"kind\":\"human\"},{\"kind\":\"human\"},"
		 "{\"kind\":\"human\"}]}", "1..8 humans"},
		{"{\"game_type\":\"single\",\"players\":[{\"kind\":\"human\"},{\"kind\":\"human\"}]}", "at most one human"},
		{"{\"game_type\":\"single\",\"players\":[{\"kind\":\"ai\"},{\"kind\":\"human\"}]}", "slot 0"},
		{"{\"game_type\":\"lan\"}", "game_type"},
		{"{\"turn_mode\":\"sequential\"}", "turn_mode"},
		{"{\"turn_timer\":7}", "turn_timer"},
		{"{\"autoplay_humans\":[1]}", "not a human slot"},
		{"{\"autoplay_humans\":[0,\"a\"]}", "autoplay_humans"},
		{"{\"game_options\":{\"GAMEOPTION_FIRSTMOVE\":1}}", "GAMEOPTION_FIRSTMOVE"},
		{"{\"game_type\":\"single\",\"game_options\":{\"GAMEOPTION_FIRSTMOVE\":0}}", "GAMEOPTION_FIRSTMOVE"},
		{"{\"game_options\":{\"GAMEOPTION_PITBOSS\":0}}", "GAMEOPTION_PITBOSS"},
		{"{\"game_options\":{\"GAMEOPTION_END_TURN_TIMER_ENABLED\":1}}", "END_TURN_TIMER_ENABLED"},
		{"{\"game_options\":{\"GAMEOPTION_QUICK_COMBAT\":0}}", "GAMEOPTION_QUICK_COMBAT"},
		{"{\"game_options\":{\"GAMEOPTION_FAST_HAND\":1}}", "GAMEOPTION_FAST_HAND"},
		{"{\"game_options\":{\"GAMEOPTION_SIMULTANEOUS_TURNS\":1}}", "GAMEOPTION_SIMULTANEOUS_TURNS"},
		{"{\"world_builder_map\":\"x\"}", "world_builder_map"},
		{"{\"humans\":2}", "humans"},
		{"{\"human\":{\"civ\":\"CIVILIZATION_ROME\",\"leader\":\"x\"}}", "human.leader"},
		{"{\"auto_choices\":{\"research\":1,\"nap\":1}}", "auto_choices.nap"},
		{"{\"seed\":\"abc\"}", "seed"},
		{"{\"ai_count\":99}", "ai_count"},
		{"{\"minor_civs\":50}", "minor_civs"},
		{"{\"max_turns\":-1}", "max_turns"},
		{"{\"max_turns\":10001}", "max_turns"},
		{"{\"max_turns\":\"many\"}", "max_turns"},
		{"{\"victories\":{\"VICTORY_TIME\":\"x\"}}", "victories.VICTORY_TIME"},
		{"{\"victories\":[1]}", "victories"},
		{"{\"map_options\":{\"1\":\"x\"}}", "map_options.1"},
		{"{\"minor_civs\":\"many\"}", "minor_civs"},
		{"[1,2]", "object"},
		{"{\"seed\":", "setup json"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
	{
		RtSetup s;
		const bool ok = s.Parse(bad[i].json, err);
		c.add(i == 0 ? "unknown_key_error" : "invalid_value_error", !ok && Contains(err, bad[i].mustName), "%s -> %s", bad[i].json, err.c_str());
	}
	RtSetup keep;
	keep.Parse("{\"seed\":5}", err);
	keep.Parse("{\"seed\":6,\"nope\":1}", err);
	c.add("failed_parse_keeps_fields", keep.seed == 5, "seed stays %u after a failed Parse", (unsigned)keep.seed);
}

bool SameChoice(const RtSetup& a, const RtSetup& b) { return a.civs == b.civs && a.minors == b.minors; }

void RunResolve(Check& c, RtSession& session)
{
	Database::Connection& db = *session.Db().Connection();
	std::string err;
	RtSetup a, b, other;
	a.Parse("{\"seed\":1234,\"ai_count\":5,\"minor_civs\":6}", err);
	b.Parse("{\"seed\":1234,\"ai_count\":5,\"minor_civs\":6}", err);
	bool ra = a.Resolve(db, err), rb = b.Resolve(db, err);
	c.add("resolve_ok", ra && rb && a.civs.size() == 6 && a.minors.size() == 6, "human + 5 AI, 6 city-states; %s", err.c_str());
	c.add("resolve_same_seed_same_choice", ra && rb && SameChoice(a, b), "seed 1234 twice: %s | %s", a.Describe().c_str(), b.Describe().c_str());

	int differing = 0;
	for (unsigned s = 1; s <= 5; ++s)
	{
		char json[96];
		snprintf(json, sizeof(json), "{\"seed\":%u,\"ai_count\":5,\"minor_civs\":6}", 1234 + s);
		other.Parse(json, err);
		if (other.Resolve(db, err) && !SameChoice(a, other)) ++differing;
	}
	c.add("resolve_other_seed_differs", differing >= 4, "%d of 5 other seeds choose differently", differing);

	std::set<std::string> seen;
	bool distinct = true, notHuman = true;
	for (size_t i = 0; i < a.civs.size(); ++i)
	{
		if (!seen.insert(a.civs[i].first).second) distinct = false;
		if (i > 0 && a.civs[i].first == a.resolved_human_civ) notHuman = false;
	}
	for (size_t i = 0; i < a.minors.size(); ++i)
		if (!seen.insert(a.minors[i]).second) distinct = false;
	c.add("resolve_distinct", distinct && notHuman && a.civs[0].first == "CIVILIZATION_ROME", "civs and city-states are distinct, the human keeps Rome");

	RtSetup rnd, rnd2;
	rnd.Parse("{\"seed\":31,\"human\":{\"civ\":\"random\"},\"ai_count\":3}", err);
	rnd2.Parse("{\"seed\":31,\"human\":{\"civ\":\"random\"},\"ai_count\":3}", err);
	const bool rr = rnd.Resolve(db, err) && rnd2.Resolve(db, err);
	c.add("resolve_random_human", rr && rnd.resolved_human_civ != "random" && rnd.resolved_human_civ == rnd2.resolved_human_civ && SameChoice(rnd, rnd2),
		"random human = %s, repeated: %s", rnd.resolved_human_civ.c_str(), rnd2.resolved_human_civ.c_str());

	RtSetup dup;
	dup.Parse("{\"players\":[{\"kind\":\"human\",\"civ\":\"CIVILIZATION_ROME\"},{\"kind\":\"ai\",\"civ\":\"CIVILIZATION_ROME\"}]}", err);
	c.add("resolve_duplicate_civ_error", !dup.Resolve(db, err) && Contains(err, "CIVILIZATION_ROME"), "%s", err.c_str());
	const char* const mixed = "{\"seed\":77,\"players\":[{\"kind\":\"ai\"},{\"kind\":\"human\",\"civ\":\"random\"},{\"kind\":\"human\",\"civ\":\"CIVILIZATION_ROME\"},"
		"{\"kind\":\"human\",\"civ\":\"random\"},{\"kind\":\"ai\",\"civ\":\"random\"},{\"kind\":\"ai\",\"civ\":\"CIVILIZATION_EGYPT\"}]}";
	RtSetup m1, m2;
	m1.Parse(mixed, err);
	m2.Parse(mixed, err);
	const bool mr = m1.Resolve(db, err) && m2.Resolve(db, err);
	std::set<std::string> mseen;
	bool mdistinct = mr;
	for (size_t i = 0; mr && i < m1.civs.size(); ++i) mdistinct = mdistinct && mseen.insert(m1.civs[i].first).second && !m1.civs[i].second.empty();
	c.add("resolve_players_mixed", mr && mdistinct && SameChoice(m1, m2) && m1.civs.size() == 6 && m1.civs[2].first == "CIVILIZATION_ROME" && m1.civs[5].first == "CIVILIZATION_EGYPT"
		&& m1.resolved_human_civ == m1.civs[1].first, "random/explicit mix is deterministic and distinct: %s", mr ? m1.Describe().c_str() : err.c_str());

	RtSetup badCiv, badTimer, goodTimer;
	badCiv.Parse("{\"players\":[{\"kind\":\"human\",\"civ\":\"CIVILIZATION_NOPE\"}]}", err);
	c.add("validate_unknown_player_civ", !badCiv.Validate(session, err) && Contains(err, "CIVILIZATION_NOPE"), "%s", err.c_str());
	badTimer.Parse("{\"turn_timer\":\"TURNTIMER_NOPE\"}", err);
	c.add("validate_unknown_turn_timer", !badTimer.Validate(session, err) && Contains(err, "TURNTIMER_NOPE"), "%s", err.c_str());
	goodTimer.Parse("{\"turn_timer\":\"TURNTIMER_MEDIUM\"}", err);
	c.add("validate_turn_timer_row", goodTimer.Validate(session, err), "TURNTIMER_MEDIUM: %s", err.c_str());

	RtSetup badVic, goodVic;
	badVic.Parse("{\"victories\":{\"VICTORY_NOPE\":0}}", err);
	c.add("validate_unknown_victory", !badVic.Validate(session, err) && Contains(err, "VICTORY_NOPE"), "%s", err.c_str());
	goodVic.Parse("{\"victories\":{\"VICTORY_TIME\":0,\"VICTORY_SPACE_RACE\":1},\"max_turns\":300}", err);
	c.add("validate_victory_rows", goodVic.Validate(session, err), "VICTORY_TIME, VICTORY_SPACE_RACE: %s", err.c_str());

	RtSetup defaults;
	defaults.Parse("{}", err);
	c.add("resolve_default_minors", defaults.Resolve(db, err) && defaults.minors.size() == 4 && defaults.civs.size() == 2, "Duel: 1 AI + %d city-states (DefaultMinorCivs 4)",
		(int)defaults.minors.size());
}

// Reads the PreGame back and compares with the setup.
bool ReadBack(Check& c, const char* tag, RtSession& session, const RtSetup& s, int expectedActive, int expectQuickCombatOption)
{
	CvDllGameContext* ctx = session.Context();
	std::auto_ptr<ICvPreGame1> pre(ctx->GetPreGame());
	std::string n;
	bool ok = true;
	std::string detail;
	char buf[256];
#define EXPECT(cond, ...) do { if (!(cond)) { ok = false; snprintf(buf, sizeof(buf), __VA_ARGS__); detail += buf; detail += "; "; } } while (0)

	EXPECT(pre->syncRandomSeed() == s.seed, "sync seed %u", pre->syncRandomSeed());
	EXPECT(pre->mapRandomSeed() == s.map_seed, "map seed %u", pre->mapRandomSeed());
	EXPECT(pre->gameType() == (s.Network() ? GAME_NETWORK_MULTIPLAYER : GAME_SINGLE_PLAYER), "game type %d", (int)pre->gameType());
	EXPECT((int)pre->worldSize() == ctx->GetInfoTypeForString(s.world_size.c_str(), true), "world size %d", (int)pre->worldSize());
	EXPECT(strcmp(pre->mapScriptName().c_str(), s.map_script.c_str()) == 0, "map script '%s'", pre->mapScriptName().c_str());
	EXPECT((int)pre->gameSpeed() == ctx->GetInfoTypeForString(s.game_speed.c_str(), true), "speed %d", (int)pre->gameSpeed());
	EXPECT((int)pre->era() == ctx->GetInfoTypeForString(s.start_era.c_str(), true), "era %d", (int)pre->era());
	EXPECT(pre->quickCombat() && pre->quickMovement(), "quick combat %d movement %d", pre->quickCombat(), pre->quickMovement());
	for (size_t i = 0; i < s.civs.size(); ++i)
	{
		const PlayerTypes p = (PlayerTypes)i;
		EXPECT((int)pre->civilization(p) == ctx->GetInfoTypeForString(s.civs[i].first.c_str(), true), "slot %d civ %d", (int)i, (int)pre->civilization(p));
		EXPECT((int)pre->leaderHead(p) == ctx->GetInfoTypeForString(s.civs[i].second.c_str(), true), "slot %d leader", (int)i);
		const bool human = s.players[i].human && !s.observer;
		EXPECT(pre->slotStatus(p) == (human ? SS_TAKEN : SS_COMPUTER), "slot %d status %d", (int)i, (int)pre->slotStatus(p));
		const char* hc = human ? s.handicap.c_str() : "HANDICAP_AI_DEFAULT";
		EXPECT((int)pre->handicap(p) == ctx->GetInfoTypeForString(hc, true), "slot %d handicap %d (%s)", (int)i, (int)pre->handicap(p), hc);
		if (human && s.Network())
		{
			const char* want = s.players[i].nick.empty() ? NULL : s.players[i].nick.c_str();
			EXPECT(want ? strcmp(CvPreGame::nickname(p).c_str(), want) == 0 : CvPreGame::nickname(p).size() > 0, "slot %d nickname '%s'", (int)i, CvPreGame::nickname(p).c_str());
		}
	}
	const int seat = s.Network() || s.observer ? 1 : 0;
	EXPECT(pre->slotStatus((PlayerTypes)(s.civs.size() + seat)) == SS_CLOSED || s.civs.size() + seat >= 22, "slot after the civs not closed");
	if (seat) EXPECT(pre->slotStatus((PlayerTypes)s.civs.size()) == SS_OBSERVER, "server seat / observer slot status");
	if (s.Network())
	{
		int pit = -1, dyn = -1, sim = -1, timer = -1;
		pre->GetGameOption("GAMEOPTION_PITBOSS", pit);
		pre->GetGameOption("GAMEOPTION_DYNAMIC_TURNS", dyn);
		pre->GetGameOption("GAMEOPTION_SIMULTANEOUS_TURNS", sim);
		pre->GetGameOption("GAMEOPTION_END_TURN_TIMER_ENABLED", timer);
		const bool hybrid = s.turn_mode == "hybrid";
		EXPECT(pit == 1 && timer == 0 && dyn == (hybrid ? 1 : 0) && sim == (hybrid ? 0 : 1), "options pitboss %d timer %d dynamic %d simultaneous %d", pit, timer, dyn, sim);
		EXPECT((int)ServerSeat() == (int)s.civs.size(), "server seat %d", (int)ServerSeat());
	}
	EXPECT(pre->numMinorCivs() == (int)s.minors.size(), "minors %d", pre->numMinorCivs());
	for (size_t j = 0; j < s.minors.size(); ++j)
		EXPECT((int)pre->minorCivType((PlayerTypes)(22 + j)) == ctx->GetInfoTypeForString(s.minors[j].c_str(), true), "minor %d type", (int)j);
	EXPECT((int)pre->activePlayer() == expectedActive, "active player %d", (int)pre->activePlayer());
	for (size_t i = 0; i < s.map_options.size(); ++i)
	{
		int v = -1;
		EXPECT(pre->GetMapOption(s.map_options[i].first.c_str(), v) && v == s.map_options[i].second, "map option %s=%d", s.map_options[i].first.c_str(), v);
	}
	for (size_t i = 0; i < s.game_options.size(); ++i)
	{
		const std::string& name = s.game_options[i].first;
		if (name == "GAMEOPTION_QUICK_COMBAT") continue;  // forced on, checked below
		int v = -1;
		EXPECT(pre->GetGameOption(name.c_str(), v) && v == s.game_options[i].second, "game option %s=%d", name.c_str(), v);
	}
	if (expectQuickCombatOption >= 0)
	{
		int v = -1;
		pre->GetGameOption("GAMEOPTION_QUICK_COMBAT", v);
		EXPECT(v == expectQuickCombatOption, "GAMEOPTION_QUICK_COMBAT=%d", v);
	}
#undef EXPECT
	c.add(tag, ok, "%s", ok ? "all PreGame values read back as set" : detail.c_str());
	return ok;
}

void RunApply(Check& c, RtSession& session)
{
	std::string err;
	RtSetup full;
	full.Parse(kFullJson, err);
	const bool applied = full.Apply(session, err);
	c.add("apply_full", applied, "G1-G9 with every key: %s", err.c_str());
	if (applied)
	{
		ReadBack(c, "readback_full", session, full, (int)full.civs.size(), 1);
		int tiny = -1;
		std::auto_ptr<ICvPreGame1> pre(session.Context()->GetPreGame());
		pre->GetMapOption("9", tiny);
		c.add("readback_map_option_override", tiny == 3, "map option 9 = %d (script default 2, override 3); %d options applied", tiny, (int)full.applied_map_options.size());
	}

	// B1 regression form, random human: a second Apply resets the pregame (G1) and sets Duel / Lekmap Pangaea.
	RtSetup def;
	def.Parse("{\"game_type\":\"single\",\"seed\":2024,\"human\":{\"civ\":\"random\"}}", err);
	const bool applied2 = def.Apply(session, err);
	c.add("apply_single_b1", applied2, "single game, random human: %s", err.c_str());
	if (applied2) ReadBack(c, "readback_single_b1", session, def, 0, -1);

	// v2 default: a network game (B1 form = 1 human + 1 AI), server seat after the civilizations.
	RtSetup net;
	net.Parse("{\"seed\":2024,\"human\":{\"civ\":\"random\"}}", err);
	const bool applied3 = net.Apply(session, err);
	c.add("apply_network_default", applied3, "defaults (network), random human: %s", err.c_str());
	if (applied3) ReadBack(c, "readback_network_default", session, net, (int)net.civs.size(), -1);

	// Players form: three humans (nicks, an explicit and random civs), AI, simultaneous; then the same in hybrid mode.
	for (int hybrid = 0; hybrid < 2; ++hybrid)
	{
		RtSetup pl;
		const bool parsed = pl.Parse(hybrid ? "{\"seed\":5,\"turn_mode\":\"hybrid\",\"handicap\":\"HANDICAP_EMPEROR\",\"players\":[{\"kind\":\"human\",\"civ\":\"CIVILIZATION_ROME\",\"nick\":\"player1\"},"
			"{\"kind\":\"ai\",\"civ\":\"CIVILIZATION_EGYPT\"},{\"kind\":\"human\",\"nick\":\"\xD0\xB4\xD1\x80\xD1\x83\xD0\xB3\"},{\"kind\":\"ai\"},{\"kind\":\"human\"}]}"
			: kPlayersJson, err);
		c.add(hybrid ? "parse_players_hybrid" : "parse_players_simultaneous", parsed, "%s", err.c_str());
		const bool ap = parsed && pl.Apply(session, err);
		c.add(hybrid ? "apply_players_hybrid" : "apply_players_simultaneous", ap, "%s", err.c_str());
		if (ap)
		{
			ReadBack(c, hybrid ? "readback_players_hybrid" : "readback_players_simultaneous", session, pl, (int)pl.civs.size(), -1);
			bool distinct = true, explicitOk = pl.civs[0].first == "CIVILIZATION_ROME";
			std::set<std::string> seen;
			for (size_t i = 0; i < pl.civs.size(); ++i) distinct = distinct && seen.insert(pl.civs[i].first).second;
			if (hybrid) explicitOk = explicitOk && pl.civs[1].first == "CIVILIZATION_EGYPT";
			else explicitOk = explicitOk && pl.civs[2].first == "CIVILIZATION_EGYPT";
			c.add(hybrid ? "players_civs_hybrid" : "players_civs_simultaneous", distinct && explicitOk && pl.civs.size() == pl.players.size(),
				"%d civs, distinct, explicit ones in their slots: %s", (int)pl.civs.size(), pl.Describe().c_str());
		}
	}

	// Victories and max_turns: setVictory per row, the rest stay valid; max_turns 0 is the default.
	{
		RtSetup v;
		const bool vp = v.Parse("{\"seed\":9,\"victories\":{\"VICTORY_TIME\":0,\"VICTORY_CULTURAL\":0},\"max_turns\":300}", err);
		const bool va = vp && v.Apply(session, err);
		c.add("apply_victories_max_turns", va, "%s", err.c_str());
		if (va)
		{
			CvDllGameContext* ctx = session.Context();
			std::auto_ptr<ICvPreGame1> pre(ctx->GetPreGame());
			const int time = ctx->GetInfoTypeForString("VICTORY_TIME", true), cultural = ctx->GetInfoTypeForString("VICTORY_CULTURAL", true);
			const int space = ctx->GetInfoTypeForString("VICTORY_SPACE_RACE", true), domination = ctx->GetInfoTypeForString("VICTORY_DOMINATION", true);
			c.add("readback_victories_max_turns", !pre->isVictory((VictoryTypes)time) && !pre->isVictory((VictoryTypes)cultural) && pre->isVictory((VictoryTypes)space)
				&& pre->isVictory((VictoryTypes)domination) && pre->maxTurns() == 300, "time %d cultural %d space %d domination %d max_turns %d",
				(int)pre->isVictory((VictoryTypes)time), (int)pre->isVictory((VictoryTypes)cultural), (int)pre->isVictory((VictoryTypes)space),
				(int)pre->isVictory((VictoryTypes)domination), pre->maxTurns());
		}
		RtSetup w;
		const bool wa = w.Parse("{\"seed\":9}", err) && w.Apply(session, err);
		if (wa)
		{
			CvDllGameContext* ctx = session.Context();
			std::auto_ptr<ICvPreGame1> pre(ctx->GetPreGame());
			c.add("readback_victories_default", pre->isVictory((VictoryTypes)ctx->GetInfoTypeForString("VICTORY_TIME", true)) && pre->maxTurns() == 0,
				"a setup without victories: all valid again, max_turns %d", pre->maxTurns());
		}
		RtSetup bad;
		bad.Parse("{\"victories\":{\"VICTORY_NOPE\":0}}", err);
		c.add("apply_unknown_victory_error", !bad.Apply(session, err) && Contains(err, "VICTORY_NOPE"), "%s", err.c_str());
	}

	RtSetup badType, badOpt;
	badType.Parse("{\"handicap\":\"HANDICAP_NOPE\"}", err);
	c.add("apply_unknown_type_error", !badType.Apply(session, err) && Contains(err, "HANDICAP_NOPE"), "%s", err.c_str());
	badOpt.Parse("{\"game_options\":{\"GAMEOPTION_NOPE\":1}}", err);
	c.add("apply_unknown_game_option_error", badOpt.Apply(session, err) == false && Contains(err, "GAMEOPTION_NOPE"), "%s", err.c_str());
}

}  // namespace

extern "C" int rt_selftest_setup(const gc_host* host, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;
	RunParse(c);
	if (host && host->read_file)
	{
		// Left alive on purpose, like scenario load: GC.init's objects keep statements on the DB connection.
		RtSession* session = new RtSession();
		RtSession::LoadStatus st;
		const bool loaded = session->Load(*host, st);
		c.add("load", loaded, "RtSession::Load reached %s%s%s", RtSession::StepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());
		if (loaded)
		{
			RunResolve(c, *session);
			RunApply(c, *session);
		}
	}
	else c.add("session", true, "skipped: no host read_file (pass --game)");

	char head[96];
	snprintf(head, sizeof(head), "{\"failed\":%d,\"checks\":[", c.failed);
	std::string out = std::string(head) + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
