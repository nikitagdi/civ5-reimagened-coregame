// Scenario `autochoice` (task F3; run.sh --scenario autochoice --turns N --seed S --ai A): rt_autochoice part 1 (SIMP-002).
// RtSession::Load, NewGame({"seed":S,"ai_count":A,"auto_choices":{"research":0}}) (the human in slot 0), then:
//  1. research auto-choice OFF (from the setup JSON): the first RESEARCH / FREE_TECH blocker must stop the turn (WAITING_HUMAN with that
//     blocker, "end turn" REJECTED_BLOCKED with it) and no research AUTO_CHOICE event may appear; then the scenario switches the flag on
//     (RtAutoChoice::SetFlags) and the next Pump must clear the blocker by itself with a research AUTO_CHOICE event;
//  2. N turns in all (default 60) with every part-1 flag on: the human must never wait on RESEARCH, FREE_TECH, POLICY, FREE_POLICY or
//     CHOOSE_IDEOLOGY. Other blockers are the human's (tasks F4/F5 automate some of them later): production -> the first building the city
//     can construct, else the first unit; units -> found a city with a settler that can, else fortify / sleep / skip, else
//     FORCE_END_TURN; promotion -> the first promotion the unit can take; pantheon -> the first available pantheon belief. Anything else
//     stops the run and is reported;
//  3. ideology: the human's team is moved to ERA_MODERN by a direct gamecore call (scenario only: IsTimeToChooseIdeology needs an era
//     after Industrial or 3 factories, out of reach in N turns), then up to 5 more turns must bring CHOOSE_IDEOLOGY, resolved by an
//     IDEOLOGY AUTO_CHOICE event, and the human then has a late-game policy tree.
//  Halfway (turn N/2) the scenario also grants 1 free technology and 1 free policy by direct gamecore calls (CvPlayer::chooseTech(1),
//  ChangeNumFreePolicies(1), as a wonder or policy would): FREE_TECH and FREE_POLICY must be resolved automatically too.
// run.sh runs it with --ai 1 (seed 1, 60 turns): AI leader messages to the human are declined by RtDiplo (F6, SIMP-003); without it the
// first meeting kept the AI turn open (seed 1, --ai 1: STUCK at turn 22).
// Prints "autochoice-event|turn|kind|type|rule|text", "autochoice-block|turn|blocker|action" and "autochoice-turn|..." lines.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <string>

#include "CvDllContext.h"
#include "CvPolicyClasses.h"
#include "CvReligionClasses.h"
#include "CvTechClasses.h"
#include "CvTypes.h"
#include "rt_autochoice.h"
#include "rt_lua.h"
#include "rt_selftest.h"
#include "rt_session.h"
#include "lekmod_portable/host_log.h"

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
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("autochoice: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

bool IsPart1Blocker(int b)
{
	return b == ENDTURN_BLOCKING_RESEARCH || b == ENDTURN_BLOCKING_FREE_TECH || b == ENDTURN_BLOCKING_POLICY || b == ENDTURN_BLOCKING_FREE_POLICY ||
		b == ENDTURN_BLOCKING_CHOOSE_IDEOLOGY;
}

RtSession::PumpResult PumpToHuman(RtSession& s, int budget = 64)
{
	RtSession::PumpResult r;
	for (int i = 0; i < budget; ++i)
	{
		r = s.Pump(RtSession::kMaxSlicesPerTurn);
		if (r.status != RtSession::PUMP_AI_RUNNING) break;
	}
	return r;
}

// The human's own command for a blocker outside part 1 (empty = cannot be resolved here). *force: end the turn with FORCE_END_TURN.
std::string ResolveHuman(RtSession& s, int blocking, const RtSession::BlockTarget& t, bool* force)
{
	*force = false;
	RtEngine& e = s.Engine();
	const PlayerTypes me = e.GetActivePlayer();
	CvPlayer& p = GET_PLAYER(me);
	switch (blocking)
	{
	case ENDTURN_BLOCKING_PRODUCTION:
	{
		CvCity* city = t.kind == RtSession::TARGET_CITY ? p.getCity(t.id) : NULL;
		if (!city) return "";
		for (int i = 0; i < GC.getNumBuildingInfos(); ++i)
			if (city->canConstruct((BuildingTypes)i))
			{
				e.sendPushOrder(city->GetID(), ORDER_CONSTRUCT, i, false, false, false);
				return Format("sendPushOrder(city %d, ORDER_CONSTRUCT, %s)", city->GetID(), GC.getBuildingInfo((BuildingTypes)i)->GetType());
			}
		for (int i = 0; i < GC.getNumUnitInfos(); ++i)
			if (city->canTrain((UnitTypes)i))
			{
				e.sendPushOrder(city->GetID(), ORDER_TRAIN, i, false, false, false);
				return Format("sendPushOrder(city %d, ORDER_TRAIN, %s)", city->GetID(), GC.getUnitInfo((UnitTypes)i)->GetType());
			}
		return "";
	}
	case ENDTURN_BLOCKING_UNITS:
	case ENDTURN_BLOCKING_STACKED_UNITS:
	{
		CvUnit* u = t.kind == RtSession::TARGET_UNIT ? p.getUnit(t.id) : NULL;
		if (!u) return "";
		if (u->canFound(u->plot()))
		{
			e.sendPushMission(u->GetID(), CvTypes::getMISSION_FOUND(), -1, -1, 0, false);
			return Format("sendPushMission(unit %d %s, MISSION_FOUND)", u->GetID(), u->getUnitInfo().GetType());
		}
		// Fortify / sleep last across turns. A unit left awake with moves makes the gamecore take a forced end turn back
		// (CvGame::UpdateMoves -> sendTurnUnready), so FORCE_END_TURN is only the last resort.
		MissionTypes m;
		const char* name;
		if (u->canFortify(u->plot())) { m = CvTypes::getMISSION_FORTIFY(); name = "MISSION_FORTIFY"; }
		else if (u->canSleep(u->plot())) { m = CvTypes::getMISSION_SLEEP(); name = "MISSION_SLEEP"; }
		else if (u->canHold(u->plot())) { m = CvTypes::getMISSION_SKIP(); name = "MISSION_SKIP"; }
		else if (blocking == ENDTURN_BLOCKING_UNITS)
		{
			*force = true;
			return "EndTurn(force)";
		}
		else return "";
		e.sendPushMission(u->GetID(), m, -1, -1, 0, false);
		return Format("sendPushMission(unit %d %s, %s)", u->GetID(), u->getUnitInfo().GetType(), name);
	}
	case ENDTURN_BLOCKING_UNIT_PROMOTION:
	{
		CvUnit* u = t.kind == RtSession::TARGET_UNIT ? p.getUnit(t.id) : NULL;
		if (!u) return "";
		for (int i = 0; i < GC.getNumPromotionInfos(); ++i)
			if (u->canPromote((PromotionTypes)i, -1))
			{
				e.sendDoCommand(u->GetID(), COMMAND_PROMOTION, i, -1, false);
				return Format("sendDoCommand(unit %d, COMMAND_PROMOTION, %s)", u->GetID(), GC.getPromotionInfo((PromotionTypes)i)->GetType());
			}
		return "";
	}
	case ENDTURN_BLOCKING_FOUND_PANTHEON:
	{
		std::vector<BeliefTypes> beliefs = GC.getGame().GetGameReligions()->GetAvailablePantheonBeliefs();
		if (beliefs.empty()) return "";
		e.SendFoundPantheon(me, beliefs[0]);
		return Format("SendFoundPantheon(%s)", GC.GetGameBeliefs()->GetEntry(beliefs[0])->GetType());
	}
	default:
		return "";
	}
}

std::string Num(long long v) { return Format("%lld", v); }

}  // namespace

extern "C" int rt_selftest_autochoice(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;

	RtSession* session = new RtSession();  // left alive on purpose, as in scenario `load`
	RtSession::LoadStatus ls;
	const bool loaded = session->Load(*host, ls);
	c.add("load", loaded, "boot reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());

	int doneTurns = 0, startTurn = -1, endTurn = -1, luaErrors = 0, stuck = 0, stoppedOnAuto = 0, humanCommands = 0, forced = 0;
	int reopened = 0, offWaitTurn = -1, offRejectOk = 0, offEvents = 0, onResolvedTurn = -1, ideologyTurn = -1, eraTurn = -1, freeTurn = -1;
	int kindCount[RtAutoChoiceEvent::CAPTURED_CITY + 1] = {0};
	int aiChoices = 0, fallbackChoices = 0;
	bool researchOff = true;
	std::string stopReason, eventsJson, stoppedOn, ideologyType;
	std::map<std::string, int> humanBlockers;
	RtAutoChoice* autoChoice = NULL;
	if (loaded)
	{
		RtSession::NewGameStatus st;
		const std::string setup = Format("{\"game_type\":\"single\",\"seed\":%u,\"ai_count\":%u,\"auto_choices\":{\"research\":0}}", seed, ai);
		const bool started = session->NewGame(*host, setup, st);
		c.add("new_game", started, "%s reached step %s%s%s", setup.c_str(), RtSession::NewGameStepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());
		RtScriptSystem* ss = session->Scripts();
		autoChoice = &session->AutoChoice();
		const RtAutoChoices& f0 = autoChoice->Flags(*session);
		c.add("flags_from_setup", f0.research == 0 && f0.policy == 1 && f0.ideology == 1, "auto_choices from the setup JSON: research=%d policy=%d ideology=%d", f0.research,
			f0.policy, f0.ideology);
		if (started && ss)
		{
			const int luaBefore = ss->GetErrorCount();
			const PlayerTypes me = session->Engine().GetActivePlayer();
			CvPlayer& human = GET_PLAYER(me);
			startTurn = st.turn;
			const unsigned ideologyTurns = 5;
			for (unsigned k = 0; stopReason.empty() && k < turns + ideologyTurns; ++k)
			{
				const int expectTurn = startTurn + (int)k;
				if (k >= turns && ideologyTurn >= 0) break;  // ideology phase done
				if (k == turns / 2 && freeTurn < 0)
				{
					// Free technology and free policy as a wonder or a policy would grant them (scenario-only gamecore calls; the gamecore adds
					// NOTIFICATION_FREE_TECH / NOTIFICATION_FREE_POLICY itself).
					human.chooseTech(1, "free technology (autochoice scenario)", NO_TECH);
					human.ChangeNumFreePolicies(1);
					freeTurn = expectTurn;
					printf("autochoice-grant|%d|1 free tech, 1 free policy (NumFreeTechs=%d NumFreePolicies=%d)\n", expectTurn, human.GetNumFreeTechs(), human.GetNumFreePolicies());
				}
				if (k == turns)
				{
					// Phase 3: make the ideology choice due (scenario-only gamecore call).
					const EraTypes modern = (EraTypes)GC.getInfoTypeForString("ERA_MODERN");
					GET_TEAM(human.getTeam()).SetCurrentEra(modern);
					eraTurn = expectTurn;
					printf("autochoice-era|%d|team %d -> era %d; IsTimeToChooseIdeology=%d\n", expectTurn, (int)human.getTeam(), (int)modern,
						human.GetPlayerPolicies()->IsTimeToChooseIdeology() ? 1 : 0);
				}
				RtSession::PumpResult r = PumpToHuman(*session);
				if (r.status == RtSession::PUMP_STUCK) ++stuck;
				if (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != expectTurn)
				{
					stopReason = Format("expected WAITING_HUMAN at turn %d, got %s at turn %d: %s", expectTurn, RtSession::PumpStatusName(r.status), r.turn, r.reason.c_str());
					break;
				}
				std::string actions;
				// The human's turn; the gamecore may reopen it after an accepted end turn.
				for (int reopen = 0;; ++reopen)
				{
					bool turnEnded = false;
					for (int steps = 0; stopReason.empty() && !turnEnded; ++steps)
					{
						// The choices made since the last look.
						RtAutoChoiceEvent ev;
						while (autoChoice->PopEvent(ev))
						{
							++kindCount[ev.kind];
							if (ev.ai) ++aiChoices; else ++fallbackChoices;
							if (researchOff && (ev.kind == RtAutoChoiceEvent::RESEARCH || ev.kind == RtAutoChoiceEvent::FREE_TECH)) ++offEvents;
							if (ev.kind == RtAutoChoiceEvent::IDEOLOGY && ideologyTurn < 0) { ideologyTurn = ev.turn; ideologyType = ev.type; }
							const std::string line = Format("%d|%s|%s|%s|%s", ev.turn, RtAutoChoice::KindName(ev.kind), ev.type.c_str(), ev.rule.c_str(), ev.text.c_str());
							printf("autochoice-event|%s\n", line.c_str());
							eventsJson += std::string(eventsJson.empty() ? "" : ",") + "{\"turn\":" + Num(ev.turn) + ",\"kind\":\"" + RtAutoChoice::KindName(ev.kind) +
								"\",\"type\":\"" + Escape(ev.type) + "\",\"blocker\":\"" + RtSession::BlockingName(ev.blocking) + "\",\"rule\":\"" + Escape(ev.rule) +
								"\",\"text\":\"" + Escape(ev.text) + "\"}";
						}
						if (steps > 64)
						{
							stopReason = Format("turn %d: the blocker %s did not go away after 64 commands", r.turn, RtSession::BlockingName(r.blocking));
							break;
						}
						if (r.blocking == NO_ENDTURN_BLOCKING_TYPE)
						{
							RtSession::EndTurnResult e = session->EndTurn(false);
							if (e.status != RtSession::END_TURN_ACCEPTED)
							{
								stopReason = Format("turn %d: EndTurn without a blocker was %s: %s", r.turn, RtSession::EndTurnStatusName(e.status), e.reason.c_str());
								break;
							}
							turnEnded = true;
							break;
						}
						std::string action;
						const bool researchBlocker = r.blocking == ENDTURN_BLOCKING_RESEARCH || r.blocking == ENDTURN_BLOCKING_FREE_TECH;
						if (researchOff && researchBlocker)
						{
							// Phase 1: with auto_choices.research = 0 the turn waits on the blocker.
							RtSession::EndTurnResult e = session->EndTurn(false);
							if (e.status == RtSession::END_TURN_REJECTED_BLOCKED && e.blocking == r.blocking) ++offRejectOk;
							offWaitTurn = r.turn;
							const int blockedOn = r.blocking;
							printf("autochoice-block|%d|%s|research auto OFF: WAITING_HUMAN, EndTurn -> %s (%s); switching research auto ON\n", r.turn,
								RtSession::BlockingName(r.blocking), RtSession::EndTurnStatusName(e.status), RtSession::BlockingName(e.blocking));
							RtAutoChoices f = autoChoice->Flags(*session);
							f.research = 1;
							autoChoice->SetFlags(f);
							researchOff = false;
							r = PumpToHuman(*session);
							if (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != expectTurn)
							{
								stopReason = Format("turn %d: after switching research auto on expected WAITING_HUMAN, got %s at turn %d: %s", expectTurn,
									RtSession::PumpStatusName(r.status), r.turn, r.reason.c_str());
								break;
							}
							if (r.blocking != blockedOn) onResolvedTurn = r.turn;
							actions += std::string(actions.empty() ? "" : "; ") + "research auto ON";
							continue;
						}
						if (IsPart1Blocker(r.blocking))
						{
							// With its flag on a part-1 blocker must never reach the human.
							++stoppedOnAuto;
							stoppedOn += Format("%s%d:%s", stoppedOn.empty() ? "" : ", ", r.turn, RtSession::BlockingName(r.blocking));
							stopReason = Format("turn %d: WAITING_HUMAN on %s with its auto-choice on", r.turn, RtSession::BlockingName(r.blocking));
							break;
						}
						bool force = false;
						const std::string cmd = ResolveHuman(*session, r.blocking, r.target, &force);
						++humanBlockers[RtSession::BlockingName(r.blocking)];
						if (cmd.empty())
						{
							stopReason = Format("turn %d: blocker %s is not part 1 and this scenario cannot resolve it", r.turn, RtSession::BlockingName(r.blocking));
							printf("autochoice-block|%d|%s|unresolved\n", r.turn, RtSession::BlockingName(r.blocking));
							break;
						}
						if (force)
						{
							RtSession::EndTurnResult fr = session->EndTurn(true);
							action = Format("EndTurn(force) -> %s", RtSession::EndTurnStatusName(fr.status));
							if (fr.status == RtSession::END_TURN_ACCEPTED) { ++forced; turnEnded = true; }
							else { stopReason = Format("turn %d: FORCE_END_TURN over %s was %s: %s", r.turn, RtSession::BlockingName(r.blocking),
								RtSession::EndTurnStatusName(fr.status), fr.reason.c_str()); }
						}
						else
						{
							++humanCommands;
							action = cmd;
						}
						printf("autochoice-block|%d|%s|%s\n", r.turn, RtSession::BlockingName(r.blocking), action.c_str());
						actions += std::string(actions.empty() ? "" : "; ") + RtSession::BlockingName(r.blocking) + " -> " + action;
						if (turnEnded || !stopReason.empty()) break;
						r = PumpToHuman(*session);
						if (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != expectTurn)
						{
							if (r.status == RtSession::PUMP_STUCK) ++stuck;
							stopReason = Format("turn %d: after a command expected WAITING_HUMAN, got %s at turn %d: %s", expectTurn, RtSession::PumpStatusName(r.status), r.turn,
								r.reason.c_str());
						}
					}
					if (!turnEnded) break;
					RtSession::PumpResult n;
					for (int i = 0; i < 64; ++i)
					{
						n = session->Pump(RtSession::kMaxSlicesPerTurn, true);
						if (n.status != RtSession::PUMP_AI_RUNNING || n.turnsAdvanced > 0) break;
					}
					if (n.status == RtSession::PUMP_WAITING_HUMAN && n.turnsAdvanced == 0 && n.turn == expectTurn && reopen < 3)
					{
						// The gamecore took the end turn back (CvGame::UpdateMoves / CancelActivePlayerEndTurn -> sendTurnUnready: a unit still
						// has moves after its auto-mission); the human is up again in the same turn.
						++reopened;
						actions += std::string(actions.empty() ? "" : "; ") + "turn reopened by the gamecore";
						r = n;
						continue;
					}
					if (n.turnsAdvanced != 1 || n.turn != expectTurn + 1)
					{
						if (n.status == RtSession::PUMP_STUCK) ++stuck;
						stopReason = Format("turn %d: after the end turn the game did not reach turn %d (%s at turn %d: %s)", expectTurn, expectTurn + 1,
							RtSession::PumpStatusName(n.status), n.turn, n.reason.c_str());
						break;
					}
					++doneTurns;
					endTurn = n.turn;
					printf("autochoice-turn|%d -> %d|cities %d|culture %d/%d|policies %d|techs %d|choices %d|%s\n", expectTurn, n.turn, human.getNumCities(),
						human.getJONSCulture(), human.getNextPolicyCost(), human.GetNumPolicies(), GET_TEAM(human.getTeam()).GetTeamTechs()->GetNumTechsKnown(),
						autoChoice->ChoiceCount(), actions.empty() ? "-" : actions.c_str());
					break;
				}
			}
			luaErrors = ss->GetErrorCount() - luaBefore;
		}
	}

	const bool ideologyDone = ideologyTurn >= 0 && loaded && autoChoice &&
		GET_PLAYER(session->Engine().GetActivePlayer()).GetPlayerPolicies()->GetLateGamePolicyTree() != NO_POLICY_BRANCH_TYPE;
	std::string human;
	for (std::map<std::string, int>::const_iterator it = humanBlockers.begin(); it != humanBlockers.end(); ++it)
		human += Format("%s%s x%d", human.empty() ? "" : ", ", it->first.c_str(), it->second);
	c.add("research_off_waits", offWaitTurn >= 0 && offRejectOk == 1 && offEvents == 0,
		"research auto off: WAITING_HUMAN on a research blocker at turn %d, end turn rejected with it %d time(s), %d research event(s) while off", offWaitTurn, offRejectOk,
		offEvents);
	c.add("research_on_resolves", onResolvedTurn >= 0 && onResolvedTurn == offWaitTurn && kindCount[RtAutoChoiceEvent::RESEARCH] + kindCount[RtAutoChoiceEvent::FREE_TECH] > 0,
		"after switching research auto on the blocker was cleared by an auto-choice in turn %d", onResolvedTurn);
	c.add("turns", loaded && doneTurns >= (int)turns && stopReason.empty(),
		"%d/%u human turn(s) done (turn %d -> %d)%s%s", doneTurns, turns, startTurn, endTurn, stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("never_stopped_on_auto", stoppedOnAuto == 0, "%d WAITING_HUMAN on a part-1 blocker with its flag on%s%s", stoppedOnAuto, stoppedOn.empty() ? "" : ": ",
		stoppedOn.c_str());
	c.add("research_choices", kindCount[RtAutoChoiceEvent::RESEARCH] > 0, "%d research, %d free tech choice(s)", kindCount[RtAutoChoiceEvent::RESEARCH],
		kindCount[RtAutoChoiceEvent::FREE_TECH]);
	c.add("policy_choices", kindCount[RtAutoChoiceEvent::POLICY] + kindCount[RtAutoChoiceEvent::POLICY_BRANCH] > 0, "%d policy, %d policy branch choice(s)",
		kindCount[RtAutoChoiceEvent::POLICY], kindCount[RtAutoChoiceEvent::POLICY_BRANCH]);
	int freeTechAfter = -1, freePoliciesAfter = -1;
	if (loaded && autoChoice && session->InGame())
	{
		const CvPlayer& h = GET_PLAYER(session->Engine().GetActivePlayer());
		freeTechAfter = h.GetNumFreeTechs();
		freePoliciesAfter = h.GetNumFreePolicies();
	}
	c.add("free_choices", freeTurn >= 0 && kindCount[RtAutoChoiceEvent::FREE_TECH] > 0 && freeTechAfter == 0 && freePoliciesAfter == 0,
		"1 free tech + 1 free policy granted at turn %d: %d FREE_TECH choice(s); free techs / policies left at the end: %d / %d", freeTurn,
		kindCount[RtAutoChoiceEvent::FREE_TECH], freeTechAfter, freePoliciesAfter);
	c.add("ideology_choice", ideologyDone, "era set to ERA_MODERN at turn %d; IDEOLOGY auto-choice at turn %d (%s)", eraTurn, ideologyTurn, ideologyType.c_str());
	c.add("ai_rule", aiChoices > 0, "%d choice(s) by the gamecore AI function, %d by the cheapest-tech fallback", aiChoices, fallbackChoices);
	c.add("no_stuck", stuck == 0, "%d stuck turn(s)", stuck);
	c.add("lua_errors_turns", loaded && luaErrors == 0, "%d Lua error(s) during the turns", luaErrors);

	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"seed\":" + Num(seed) + ",\"ai_count\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) +
		",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"forced\":" + Num(forced) +
		",\"human_commands\":" + Num(humanCommands) + ",\"reopened\":" + Num(reopened) + ",\"human_blockers\":\"" + Escape(human) + "\",\"stop_reason\":\"" + Escape(stopReason) + "\",\"events\":[" +
		eventsJson + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("autochoice-summary|seed %u|ai %u|turns %d|reopened %d|research %d|free tech %d|policy %d|branch %d|ideology %d|human blockers %s\n", seed, ai, doneTurns, reopened,
		kindCount[RtAutoChoiceEvent::RESEARCH], kindCount[RtAutoChoiceEvent::FREE_TECH], kindCount[RtAutoChoiceEvent::POLICY], kindCount[RtAutoChoiceEvent::POLICY_BRANCH],
		kindCount[RtAutoChoiceEvent::IDEOLOGY], human.empty() ? "none" : human.c_str());
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
