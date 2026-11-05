// Scenario `s2` (S2, task N3.16; run.sh --scenario s2 --humans 4 --ai 2 --turns 100 [--game-type single]): what is lost when the core's phases run with the
// server seat as the active player (audit C5 F-01, F-02, F-03, F-11, F-13). One game through the C ABI as `mp` does (test bots play the humans), counting per
// human; --game-type single runs the B1 reference (one human who is the active player), whose figures are the rate to compare with. Nothing here changes the game.
//   M1 (F-01)  research_idle        human turns that reach the human phase with no current research (and a city) although a technology can be researched: the research blocker
//                                   (a notification) was not made, so nothing asked the human and his science went nowhere that turn; research_blockers = turns with the blocker.
//   M2 (F-02)  ended_ready          human turns in which a unit needed orders after the human had ended the turn (CvGame::UpdateMoves would re-open the turn for the local player only);
//                                   single game: the bot's `retaken` count, the same thing.
//   M3 (F-02)  captured / popups    cities a human got that were not founded by him (conquest, buyout) against the CAPTURED_CITY choices made for him (the popup path).
//   M4 (F-02)  picker_units / goody ruin rewards: units of the humans with the promotion that asks for the ruin reward (the others get a random one in every game type) and the rewards asked.
//   M5 (F-03)  resource_expected / resource_notes   NOTIFICATION_DISCOVERED_*_RESOURCE a human should have got when a technology he learned revealed a resource in his land
//                                   (counted from the plots at the turn after: an estimate) against those that reached him (CvTeam.cpp:5784, only for the active team).
//   M6 (F-11)  diplo messages from the AI to each human (requests, offers, others) and how many reached the human at all.
//   M7         messages and popups per human (events MESSAGE, POPUP_INFO), notifications by type.
// F-13 (getters that write) is measured by `purity`, not here.
// Prints "s2-metric|name|player|value" lines and one "s2-summary|..." line.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "CvTechClasses.h"
#include "rt_active.h"
#include "rt_autoplay.h"
#include "rt_autochoice.h"
#include "rt_diplo.h"
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
		printf("s2: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings, luaErrors;
	// by player
	int notes[MAX_PLAYERS], messages[MAX_PLAYERS], popups[MAX_PLAYERS], autoChoices[MAX_PLAYERS], declines[MAX_PLAYERS];
	std::map<std::pair<int, int>, int> notesByType;   // (player, notification type) -> count
	std::map<std::pair<int, int>, int> popupsByType;  // (player, popup type)
	std::map<std::pair<int, int>, int> choicesByKind; // (player, auto-choice kind)
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0)
	{
		memset(notes, 0, sizeof(notes));
		memset(messages, 0, sizeof(messages));
		memset(popups, 0, sizeof(popups));
		memset(autoChoices, 0, sizeof(autoChoices));
		memset(declines, 0, sizeof(declines));
	}
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
	if (ev->kind == GC_EVENT_NOTIFICATION_ADDED && ev->argc >= 7 && ev->args[6] >= 0 && ev->args[6] < MAX_PLAYERS)
	{
		++s->notes[ev->args[6]];
		++s->notesByType[std::make_pair(ev->args[6], ev->args[1])];
	}
	if (ev->kind == GC_EVENT_MESSAGE && ev->argc >= 1 && ev->args[0] >= 0 && ev->args[0] < MAX_PLAYERS) ++s->messages[ev->args[0]];
	if (ev->kind == GC_EVENT_POPUP_INFO && ev->argc >= 5 && ev->args[4] >= 0 && ev->args[4] < MAX_PLAYERS)
	{
		++s->popups[ev->args[4]];
		++s->popupsByType[std::make_pair(ev->args[4], ev->args[0])];
	}
	if (ev->kind == GC_EVENT_AUTO_CHOICE && ev->argc >= 2 && ev->args[1] >= 0 && ev->args[1] < MAX_PLAYERS)
	{
		++s->autoChoices[ev->args[1]];
		++s->choicesByKind[std::make_pair(ev->args[1], ev->args[0])];
	}
	if (ev->kind == GC_EVENT_DIPLO_AUTO_DECLINED && ev->argc >= 3 && ev->args[2] >= 0 && ev->args[2] < MAX_PLAYERS) ++s->declines[ev->args[2]];
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 20) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
}

struct PerHuman
{
	int researchIdle, researchBlockers, humanTurns;
	int endedReady;
	bool seenPhase, idleNow, readyNow;  // of the turn being looked at; flushed into the counters at the turn change
	std::set<std::pair<int, int> > endedReadySeen;  // (turn, 0) once per turn
	int capturedCities, pickerUnits;
	std::set<int> knownCities;
	std::set<int> knownTechs;
	int resourceExpected;
	std::set<std::pair<int, int> > turnsSampled;
	PerHuman() : researchIdle(0), researchBlockers(0), humanTurns(0), endedReady(0), seenPhase(false), idleNow(false), readyNow(false), capturedCities(0), pickerUnits(0), resourceExpected(0) {}
};

// Resource plots a technology reveals inside the land of `p`, visible to its team now (an estimate of what the notification of CvTeam.cpp:5784 counted).
int ResourcePlotsRevealed(CvPlayer& p, TechTypes tech)
{
	int n = 0;
	CvMap& map = GC.getMap();
	for (int i = 0; i < map.numPlots(); ++i)
	{
		CvPlot* plot = map.plotByIndex(i);
		if (!plot || plot->getOwner() != p.GetID() || !plot->isRevealed(p.getTeam(), false)) continue;
		const ResourceTypes r = plot->getResourceType();
		if (r == NO_RESOURCE) continue;
		const CvResourceInfo* info = GC.getResourceInfo(r);
		if (info && (TechTypes)info->getTechReveal() == tech && plot->getResourceType(p.getTeam()) != NO_RESOURCE) ++n;
	}
	return n;
}

}  // namespace

extern "C" int rt_selftest_s2(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* game_type, const char* world_size, char* report, size_t report_len)
{
	const bool network = game_type && !strcmp(game_type, "network");
	if (!network) humans = 1;
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	int startTurn = -1, endTurn = -1, doneTurns = 0, stuck = 0, gameOver = 0, pumpErrors = 0, calls = 0;
	std::string stopReason;
	std::vector<int> humanList;
	std::vector<PerHuman> st(MAX_PLAYERS);
	std::map<int, int> diploToRequests, diploToOffers, diploToOther;
	int notesResourceTotal[MAX_PLAYERS];
	memset(notesResourceTotal, 0, sizeof(notesResourceTotal));
	if (inst)
	{
		std::string setup;
		if (network)
		{
			std::string players;
			for (unsigned i = 0; i < humans; ++i)
				players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
			for (unsigned i = 0; i < ai; ++i) players += std::string(humans || i ? "," : "") + "{\"kind\":\"ai\",\"civ\":\"random\"}";
			std::string slots;
			for (unsigned i = 0; i < humans; ++i) slots += Format("%s%u", i ? "," : "", i);
			setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"autoplay_humans\":[%s],\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed, players.c_str(),
				slots.c_str(), world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		}
		else
			setup = Format("{\"seed\":%u,\"game_type\":\"single\",\"ai_count\":%u,\"autoplay_human\":1,\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed, ai + 1, world_size && *world_size ? ",\"world_size\":\"" : "",
				world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s", setup.c_str(), rc, err.message);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			if (network)
			{
				for (int i = 0; i < MAX_PLAYERS; ++i)
					if (IsSeatedHuman(i)) humanList.push_back(i);
			}
			else
				humanList.push_back((int)GC.getGame().getActivePlayer());
			for (size_t h = 0; h < humanList.size(); ++h)
			{
				CvPlayer& p = GET_PLAYER((PlayerTypes)humanList[h]);
				for (int t = 0; t < GC.getNumTechInfos(); ++t)
					if (GET_TEAM(p.getTeam()).GetTeamTechs()->HasTech((TechTypes)t)) st[humanList[h]].knownTechs.insert(t);
			}
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = GC.getGame().getGameTurn();
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
				const int now = GC.getGame().getGameTurn();
				if (now != turn)
				{
					// The turn changed: what the humans learned and got since the last look.
					for (size_t h = 0; h < humanList.size(); ++h)
					{
						const int who = humanList[h];
						CvPlayer& p = GET_PLAYER((PlayerTypes)who);
						PerHuman& s = st[who];
						if (s.seenPhase)
						{
							++s.humanTurns;
							if (s.idleNow) ++s.researchIdle;
							if (s.readyNow) ++s.endedReady;
						}
						s.seenPhase = s.idleNow = s.readyNow = false;
						for (int t = 0; t < GC.getNumTechInfos(); ++t)
							if (GET_TEAM(p.getTeam()).GetTeamTechs()->HasTech((TechTypes)t) && s.knownTechs.insert(t).second) s.resourceExpected += ResourcePlotsRevealed(p, (TechTypes)t);
						int loop;
						for (CvCity* city = p.firstCity(&loop); city; city = p.nextCity(&loop))
							if (s.knownCities.insert(city->GetID()).second && city->getOriginalOwner() != p.GetID()) ++s.capturedCities;
						for (CvUnit* u = p.firstUnit(&loop); u; u = p.nextUnit(&loop))
							if (u->isHasPromotion((PromotionTypes)GC.getPROMOTION_GOODY_HUT_PICKER())) s.pickerUnits = std::max(s.pickerUnits, 1);
					}
					doneTurns += now - turn;
					turn = now;
					if (turn >= target) break;
				}
				// The human phase of the turn: research state of every human at its first look.
				const bool phase = session->HumanPhase();
				for (size_t h = 0; phase && h < humanList.size(); ++h)
				{
					const int who = humanList[h];
					CvPlayer& p = GET_PLAYER((PlayerTypes)who);
					PerHuman& s = st[who];
					if (!p.isAlive()) continue;
					const bool ended = network ? session->Engine().TurnCompleteSent((PlayerTypes)who) : session->Engine().HasSentTurnComplete();
					// F-01: what the last look of the turn found (the human has ended the turn, or is about to): no research chosen and nothing asked him (the research blocker would have).
					s.seenPhase = true;
					{
						CvPlayerTechs* techs = p.GetPlayerTechs();
						bool canResearch = false;
						for (int t = 0; t < GC.getNumTechInfos() && !canResearch; ++t) canResearch = techs->CanResearch((TechTypes)t);
						s.idleNow = techs->GetCurrentResearch() == NO_TECH && p.getNumCities() > 0 && canResearch && p.GetNumFreeTechs() == 0;
					}
					// F-02: a unit needing orders after the human has ended the turn.
					if (ended && p.hasReadyUnit()) s.readyNow = true;
				}
			}
			endTurn = turn;
			// A single game: the bot's re-opened turns are the same thing as M2.
			if (!network)
			{
				const RtAutoplay* b = InstanceBot(inst, humanList[0]);
				if (b) st[humanList[0]].endedReady = std::max(st[humanList[0]].endedReady, b->Stats().retaken);
			}
			// diplomacy to the humans
			const std::vector<RtDiploEvent>& evs = session->Diplo().Events();
			for (size_t i = 0; i < evs.size(); ++i)
			{
				if (evs[i].state == DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER) ++diploToOffers[evs[i].to];
				else if (evs[i].request) ++diploToRequests[evs[i].to];
				else ++diploToOther[evs[i].to];
			}
		}
	}

	// The notifications of the resource kinds by human.
	const int kResTypes[3] = {(int)NOTIFICATION_DISCOVERED_LUXURY_RESOURCE, (int)NOTIFICATION_DISCOVERED_STRATEGIC_RESOURCE, (int)NOTIFICATION_DISCOVERED_BONUS_RESOURCE};
	for (size_t h = 0; h < humanList.size(); ++h)
		for (int k = 0; k < 3; ++k)
		{
			std::map<std::pair<int, int>, int>::const_iterator it = seen.notesByType.find(std::make_pair(humanList[h], kResTypes[k]));
			if (it != seen.notesByType.end()) notesResourceTotal[humanList[h]] += it->second;
		}
	long long sIdle = 0, sBlockers = 0, sTurns = 0, sEnded = 0, sCaptured = 0, sCapturedChoices = 0, sPicker = 0, sGoody = 0, sResExp = 0, sResGot = 0, sReq = 0, sOffers = 0, sOther = 0, sDeclined = 0, sMsg = 0, sPop = 0, sNotes = 0;
	for (size_t h = 0; h < humanList.size(); ++h)
	{
		const int who = humanList[h];
		const int capChoices = seen.choicesByKind.count(std::make_pair(who, (int)RtAutoChoiceEvent::CAPTURED_CITY)) ? seen.choicesByKind[std::make_pair(who, (int)RtAutoChoiceEvent::CAPTURED_CITY)] : 0;
		st[who].researchBlockers = seen.choicesByKind.count(std::make_pair(who, (int)RtAutoChoiceEvent::RESEARCH)) ? seen.choicesByKind[std::make_pair(who, (int)RtAutoChoiceEvent::RESEARCH)] : 0;
		const int goody = seen.choicesByKind.count(std::make_pair(who, (int)RtAutoChoiceEvent::GOODY_HUT)) ? seen.choicesByKind[std::make_pair(who, (int)RtAutoChoiceEvent::GOODY_HUT)] : 0;
		const PerHuman& s = st[who];
		printf("s2-human|player %d|human turns %d|research idle %d|research choices made for him %d|ended with a unit needing orders %d|cities got by others' founding %d (CAPTURED_CITY choices %d)|picker units %d, ruin rewards asked %d|"
			"resource notes expected %d got %d|AI requests %d, offers %d, other messages %d (declined events %d)|messages %d|popups %d|notifications %d|auto-choices %d\n",
			who, s.humanTurns, s.researchIdle, s.researchBlockers, s.endedReady, s.capturedCities, capChoices, s.pickerUnits, goody, s.resourceExpected, notesResourceTotal[who], diploToRequests[who],
			diploToOffers[who], diploToOther[who], seen.declines[who], seen.messages[who], seen.popups[who], seen.notes[who], seen.autoChoices[who]);
		sIdle += s.researchIdle; sBlockers += s.researchBlockers; sTurns += s.humanTurns; sEnded += s.endedReady; sCaptured += s.capturedCities; sCapturedChoices += capChoices; sPicker += s.pickerUnits;
		sGoody += goody; sResExp += s.resourceExpected; sResGot += notesResourceTotal[who]; sReq += diploToRequests[who]; sOffers += diploToOffers[who]; sOther += diploToOther[who];
		sDeclined += seen.declines[who]; sMsg += seen.messages[who]; sPop += seen.popups[who]; sNotes += seen.notes[who];
		for (std::map<std::pair<int, int>, int>::const_iterator it = seen.popupsByType.begin(); it != seen.popupsByType.end(); ++it)
			if (it->first.first == who) printf("s2-popup|player %d|popup type %d|%d\n", who, it->first.second, it->second);
		for (std::map<std::pair<int, int>, int>::const_iterator it = seen.notesByType.begin(); it != seen.notesByType.end(); ++it)
			if (it->first.first == who) printf("s2-note|player %d|type %u|%d\n", who, (unsigned)it->first.second, it->second);
	}
	printf("s2-total|%s|humans %d|human turns %lld|research idle %lld|research choices %lld|ended with a unit needing orders %lld|cities by others' founding %lld (CAPTURED_CITY choices %lld)|picker units %lld, ruin rewards asked %lld|"
		"resource notes expected %lld got %lld|AI requests %lld, offers %lld, other messages %lld|messages %lld|popups %lld|notifications %lld\n",
		network ? "network" : "single", (int)humanList.size(), sTurns, sIdle, sBlockers, sEnded, sCaptured, sCapturedChoices, sPicker, sGoody, sResExp, sResGot, sReq, sOffers, sOther, sMsg, sPop, sNotes);

	c.add("turns", doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) (turn %d -> %d), %d gc_pump call(s)%s%s", doneTurns, turns, startTurn, endTurn, calls, stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0 && gameOver == 0 && pumpErrors == 0, "%d STUCK, %d GAME_OVER, %d gc_pump error(s)", stuck, gameOver, pumpErrors);
	c.add("humans", (int)humanList.size() == (int)humans && sTurns > 0, "%d human(s), %lld human turn(s) looked at", (int)humanList.size(), sTurns);
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("s2-event|%s\n", seen.lines[i].c_str());
	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"game_type\":\"" + (network ? "network" : "single") + "\",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) +
		",\"turns_done\":" + Num(doneTurns) + ",\"human_turns\":" + Num(sTurns) + ",\"research_idle\":" + Num(sIdle) + ",\"research_blockers\":" + Num(sBlockers) + ",\"ended_ready\":" + Num(sEnded) +
		",\"captured_cities\":" + Num(sCaptured) + ",\"captured_choices\":" + Num(sCapturedChoices) + ",\"ruin_rewards\":" + Num(sGoody) + ",\"resource_expected\":" + Num(sResExp) + ",\"resource_got\":" + Num(sResGot) +
		",\"ai_requests\":" + Num(sReq) + ",\"ai_offers\":" + Num(sOffers) + ",\"messages\":" + Num(sMsg) + ",\"popups\":" + Num(sPop) + ",\"notifications\":" + Num(sNotes) + ",\"log_errors\":" + Num(seen.logErrors) +
		",\"lua_errors\":" + Num(seen.luaErrors) + ",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("s2-summary|%s|seed %u|humans %u|ai %u|turns %d/%u|%s\n", network ? "network" : "single", seed, humans, ai, doneTurns, turns, stopReason.c_str());
	if (inst) gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
