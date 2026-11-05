// rt_autoplay: the test bot in the human's seat (task F8, milestone M4). Only for tests and autoplay:
// in the game a person plays the human. It is switched on by the setup's "autoplay_human": 1; gc_pump then calls Act() every time the
// runtime reports WAITING_HUMAN and turns that status into AI_RUNNING (the host keeps pumping), or into STUCK when the bot cannot go on.
//
// The bot sees the game only through the ABI queries (Q_UNITS, Q_UNIT_ACTIONS, Q_CITIES, Q_CITY_BUILDS, Q_PATH) and acts only through the
// human commands of gc_apply_command (ApplyHumanCommand: the same validation and loopback as the UI), so a run walks the command and
// blocker paths a player would. Target picking (a settle plot, an enemy next to a unit) reads the gamecore read-only. The choices without a
// screen are left to rt_autochoice (SIMP-002) and the diplomacy to rt_diplo (SIMP-003), as for a human without those screens.
//
// One Act() is one "round": the commands it gives are delivered by the next Pump.
//  1. cities without production: CITY_SET_PRODUCTION from Q_CITY_BUILDS (enabled items only) by a fixed rule: a settler while the empire
//     has fewer than kTargetCities cities (settlers counted), a worker per city, two land military units per city (the dearest enabled one,
//     i.e. the newest), then the cheapest enabled building, a wonder, any unit, a process;
//  2. once per turn: a fortified / sleeping military unit with an attack target (below) is woken (UNIT_WAKE) so it can attack next round;
//  3. units that need orders (Q_UNITS NEEDS_ORDERS and CAN_MOVE), by what Q_UNIT_ACTIONS allows:
//     settler: FOUND_CITY when enabled; else MOVE_TO the gamecore's best settle plot for it (CvPlayer::GetBestSettlePlot), else the nearest
//       plot it can found on within kSettleRadius that has a path; else SKIP;
//     worker / work boat: AUTOMATE_BUILD; scout (default AI UNITAI_EXPLORE): AUTOMATE_EXPLORE while it is allowed;
//     military: RANGE_ATTACK the first enemy (at war) in range; a melee unit with at least 60% hit points attacks (MOVE_TO) an adjacent
//       enemy unit when its attack strength is at least 1.2x the defender's; HEAL below half hit points; else FORTIFY / SLEEP / SKIP;
//     other civilians: SLEEP / SKIP;
//     when nothing else is allowed (STACKED_UNITS): MOVE_TO the first adjacent plot with a path.
//     A unit ordered more than kMaxOrdersPerUnit times in one turn only gets SKIP.
//  4. nothing to order: END_TURN when there is no blocker; FORCE_END_TURN for a UNITS / STACKED_UNITS blocker the round could not
//     answer; any other blocker is left for the next round (the auto-choices answer it in the pump).
// STUCK: a human turn with more than kMaxRounds rounds, or kMaxIdleRounds rounds in a row without a command; the reason names the blocker.
#pragma once

#include <map>
#include <string>

namespace lekmod_portable {

class RtSession;

struct RtAutoplayStats
{
	int turns;              // human turns the bot ended (END_TURN / FORCE_END_TURN accepted)
	int rounds;             // Act() calls
	int commands;           // commands accepted by ApplyHumanCommand
	int rejected;           // commands ApplyHumanCommand refused (the bot then tries a fallback)
	int forced;             // FORCE_END_TURN accepted
	int byKind[48];         // accepted commands per GC_CMD_* kind
	int founded;            // FOUND_CITY accepted
	int attacks;            // melee attacks (MOVE_TO onto an enemy) and RANGE_ATTACK accepted
	int productions;        // CITY_SET_PRODUCTION accepted
	int maxRoundsPerTurn;   // most rounds one human turn took
	int retaken;            // turns the gamecore took back after an accepted end turn (the bot was asked again in the same turn)
	std::string firstReject;  // the first refusal text
	std::string firstRetaken; // what the first taken-back turn looked like
	RtAutoplayStats();
};

class RtAutoplay
{
public:
	static const int kTargetCities = 4;
	static const int kSettleRadius = 10;
	static const int kMaxOrdersPerUnit = 4;
	static const int kMaxRounds = 300;
	static const int kMaxIdleRounds = 60;

	RtAutoplay();

	// One round for the human (call only when RtSession::Pump returned WAITING_HUMAN). False = the bot cannot go on: why says why.
	bool Act(RtSession& s, std::string& why);
	// Spike S1: one round for human `player` of a network game, with that human as the active player (ActiveAs) for
	// its queries, commands and end turn. One RtAutoplay object per human (the per-turn state is the human's).
	bool ActAs(RtSession& s, int player, std::string& why);

	const RtAutoplayStats& Stats() const { return m_stats; }

private:
	friend struct RtSaveAccess;  // rt_save.cpp (G4a): the runtime state a save carries
	struct Ctx;
	void NewTurn(int turn);
	int SetProductions(RtSession& s, Ctx& c);
	int WakeForAttacks(RtSession& s, Ctx& c);
	int OrderUnits(RtSession& s, Ctx& c);
	// Applies one command for the human; counts it. True when accepted.
	bool Command(RtSession& s, int kind, int a0, int a1, int a2, int argc, int a3 = 0);

	RtAutoplayStats m_stats;
	int m_turn;
	int m_endedTurn;  // the last turn the bot ended (-1 = none)
	int m_roundsThisTurn;
	int m_idleRounds;
	bool m_wokeThisTurn;
	std::map<int, int> m_ordersThisTurn;  // unit id -> orders given this turn
};

}  // namespace lekmod_portable
