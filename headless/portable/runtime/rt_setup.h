// rt_setup: game setup JSON -> CvPreGame steps G1-G9.
// Port of headless/host/src/game/HstScenario.{h,cpp} (ApplyPreGame) with the JSON keys of the gc ABI instead of the civhost
// scenario file, plus a seeded random choice of the AI civilizations and city-states (new; the exe's own random choice,
// RVA 0x10b090, is not reproduced). Parse() and Resolve() do not touch the gamecore; Apply() needs a loaded RtSession and
// stops before Init / Init2 (starting the game is rt_session's job, task A8).
#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "CvGameDatabase.h"

namespace lekmod_portable {

class RtSession;

struct RtAutoChoices
{
	// Flags of "auto_choices" (ABI 4.2); consumed by rt_autochoice, only parsed and stored here. Default 1 = automatic.
	int research, policy, ideology, religion, promotion, great_person, captured_city, league, archaeology, goody_hut, steal_tech;
	RtAutoChoices()
		: research(1), policy(1), ideology(1), religion(1), promotion(1), great_person(1), captured_city(1), league(1), archaeology(1), goody_hut(1),
		  steal_tech(1) {}
};

// One slot of "players"; the slot number is the index.
struct RtPlayerSpec
{
	bool human;          // kind "human" (SS_TAKEN) or "ai" (SS_COMPUTER)
	std::string civ;     // a CIVILIZATION_* type or "random"
	std::string nick;    // humans only, UTF-8 up to 64 bytes; empty = a default nickname
	RtPlayerSpec() : human(false) {}
};

struct RtSetup
{
	// ---- parsed (defaults = the ABI 4.2 example) ----
	uint32_t seed;                 // "seed": CvPreGame sync random seed and the seed of the AI / city-state choice
	uint32_t map_seed;             // "map_seed": map random seed; absent = seed
	std::string human_civ;         // B1 form "human": {"civ"}; "random" = a random playable civilization (from seed). After Parse: the first human's civ
	int ai_count;                  // B1 form "ai_count": AI major civilizations, human not counted. After Parse: the number of AI slots of "players"
	std::string world_size;        // "world_size"
	std::string map_script;        // "map_script", '/' is turned into '\' (MapScriptOptions.FileName)
	std::vector<std::pair<std::string, int> > map_options;   // "map_options": OptionID -> value, over the script's defaults
	std::string handicap;          // "handicap": the human; AI slots get HANDICAP_AI_DEFAULT
	std::string game_speed;        // "game_speed"
	std::string start_era;         // "start_era"
	bool minors_default;           // "minor_civs": "default" = DefaultMinorCivs of the world size
	int minor_count;               // "minor_civs": a number
	std::vector<std::string> minor_list;   // "minor_civs": an array of MINOR_CIV_* types
	bool minors_listed;
	std::vector<std::pair<std::string, int> > game_options;  // "game_options": GAMEOPTION_* -> value
	// Lobby options (login-lobby packet P2): victory conditions and the turn limit of the staging room.
	std::vector<std::pair<std::string, int> > victories;     // "victories": VICTORY_* -> 0/1 (PreGame setVictory per row of the Victories table); unlisted ones keep the default (all on)
	int max_turns;                 // "max_turns": PreGame setMaxTurns; 0 = the game speed's default (no limit of the lobby)
	RtAutoChoices auto_choices;
	int diplo_auto_decline;       // "diplo_auto_decline" (rt_autochoice)
	int autoplay_human;            // "autoplay_human" (rt_autoplay, B1: the human of slot 0); also set to 1 when "autoplay_humans" is not empty
	int observer;                  // "observer": 1 = an SS_OBSERVER slot after the civilizations is the active player
	// (package P2):
	std::string game_type;         // "game_type": "network" (default: GAME_NETWORK_MULTIPLAYER + Pitboss + server seat) or "single" (B1 regression only)
	std::vector<RtPlayerSpec> players;   // "players" (slot order); the B1 form ("human" + "ai_count") is turned into it by Parse
	std::string turn_mode;         // "turn_mode": "hybrid" (GAMEOPTION_DYNAMIC_TURNS) or "simultaneous" (GAMEOPTION_SIMULTANEOUS_TURNS); network only
	std::string turn_timer;        // "turn_timer": a TurnTimers row type or "none"; only validated and kept here (the brain has no clock: the server expires turns, Q_GAME.turn_len_s / turn_base_s report the row)
	std::vector<int> autoplay_humans;    // "autoplay_humans": human slots played by the test bot (tests only)
	// (package P14): a WorldBuilder map instead of the map script.
	std::string world_builder_map;       // "world_builder_map": game-data path of a .Civ5Map ("" = the map script); the world size is the map's
	int world_builder_scenario;          // "world_builder_scenario": 0 = the map only, 1 = also its scenario (players, cities, units, options)
	bool world_size_given;               // "world_size" was in the JSON (an error together with world_builder_map)
	bool Network() const { return game_type == "network"; }
	bool WorldBuilder() const { return !world_builder_map.empty(); }
	int HumanCount() const;        // human slots of "players"

	// ---- filled by Resolve() ----
	std::string resolved_human_civ;                          // civilization of the first human (slot 0 in B1)
	std::vector<std::pair<std::string, std::string> > civs;  // civilization / leader types, one per "players" slot (same order)
	std::vector<std::string> minors;                          // chosen MINOR_CIV_* types, in the order of the minor slots
	std::vector<std::pair<std::string, int> > applied_map_options;  // filled by Apply (script defaults, then overrides)

	RtSetup();

	// JSON text -> fields. Every key of ABI 4.2; an unknown key (also inside "human" / "auto_choices") or a value of the wrong
	// type is an error that names it ("unknown key 'foo'"). The caller maps an error to GC_ERR_INVALID_ARG.
	bool Parse(const std::string& text, std::string& error);

	// Random choices from `seed`, deterministic: the explicit civilizations are taken first (a civilization twice is an error), then each "random"
	// human in slot order draws a distinct playable one (Civilizations.Playable), then the "random" AI slots draw distinct AI civilizations
	// (Civilizations.AIPlayable, in draw order; one leader each from Civilization_Leaders), then the city-states (MinorCivilizations).
	// For the B1 form this is the draw sequence of B1. Fills the resolved fields.
	bool Resolve(Database::Connection& db, std::string& error);

	// Names against the DB of the loaded session (world_size, map_script, human civ, handicap, game_speed, start_era, minor_civs list): an unknown one
	// is an error naming the key and the value ("start_era 'ERA_X' is unknown"). Nothing is touched; gc_new_game calls it before the game slot is taken.
	bool Validate(RtSession& session, std::string& error) const;

	// G1-G9 on ICvPreGame1 of the loaded session (calls Resolve when it has not run yet). Does not start the game.
	bool Apply(RtSession& session, std::string& error);

	// After game->Init (L3), before Init2: a WorldBuilder scenario sets its options and start year once more (CvGame::init resets the start
	// year), then the options the brain sets itself are put back. No-op without a scenario.
	void AfterGameInit() const;

	// One line per setting for the log.
	std::string Describe() const;

private:
	void ForceGameOptions() const;                                       // "game_options" and the brain's options over a scenario's
	void ApplyVictoriesAndTurns() const;                                 // "victories" and "max_turns" (a scenario sets its own first)
	bool SetupScenarioPlayers(RtSession& session, std::string& error);  // P14: the scenario's players, leaders and city-states
	bool m_resolved;
};

}  // namespace lekmod_portable
