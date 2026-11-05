// Ported from headless/host/src/engine/HstUserInterface.h (HostLeaderMessage) and headless/tools/civsim/lockstep/bot/bot.lua (leaderMessage,
// the trade-screen close / refusal paths of the exe's LeaderHead UI). See rt_diplo.h.
#include "CvGameCoreDLLPCH.h"

#include <stdio.h>

#include "CvDealClasses.h"
#include "CvDealAI.h"
#include "CvDiplomacyRequests.h"
#include "CvDllDeal.h"
#include "rt_active.h"
#include "rt_diplo.h"
#include "lekmod_portable/host_log.h"

namespace lekmod_portable {

RtDiplo::RtDiplo() : m_total(0), m_processing(false), m_scratch(NULL)
{
	for (int i = 0; i < NUM_RTDIPLO_ANSWERS; ++i) m_answers[i] = 0;
}

RtDiplo::~RtDiplo() { delete m_scratch; }

void RtDiplo::Push(PlayerTypes from, DiploUIStateTypes state, const char* text, LeaderheadAnimationTypes anim, int data)
{
	Message m;
	m.turn = GC.getGame().getGameTurn();
	m.from = (int)from;
	m.to = (int)GC.getGame().getActivePlayer();
	m.state = (int)state;
	m.anim = (int)anim;
	m.data = data;
	m.text = text ? text : "";
	m_pending.push_back(m);
}

void RtDiplo::SetScratchDeal(ICvDeal1* deal)
{
	delete m_scratch;
	m_scratch = NULL;
	if (!deal) return;
	CvDeal* inst = static_cast<CvDllDeal*>(deal)->GetInstance();
	if (inst) m_scratch = new CvDeal(*inst);
}

namespace {

// The button-less answer of every discussion the exe's DiscussionDialog shows with buttons: the *_RESPONSE event of the state.
// Button 2 is the refusing one in every response handler of CvDiplomacyAI::DoFromUIDiploEvent ("no", "tell the AI to die", "sorry, no");
// -1 for a state without a response event.
int ResponseEvent(int state)
{
	switch (state)
	{
	case DIPLO_UI_STATE_DISCUSS_AGGRESSIVE_MILITARY_WARNING: return FROM_UI_DIPLO_EVENT_AGGRESSIVE_MILITARY_WARNING_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_I_ATTACKED_YOUR_MINOR_CIV: return FROM_UI_DIPLO_EVENT_I_ATTACKED_YOUR_MINOR_CIV_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_I_BULLIED_YOUR_MINOR_CIV: return FROM_UI_DIPLO_EVENT_I_BULLIED_YOUR_MINOR_CIV_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_YOU_ATTACKED_MINOR_CIV: return FROM_UI_DIPLO_EVENT_ATTACKED_MINOR_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_YOU_KILLED_MINOR_CIV: return FROM_UI_DIPLO_EVENT_KILLED_MINOR_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_YOU_BULLIED_MINOR_CIV: return FROM_UI_DIPLO_EVENT_BULLIED_MINOR_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_YOU_EXPANSION_SERIOUS_WARNING: return FROM_UI_DIPLO_EVENT_EXPANSION_SERIOUS_WARNING_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_YOU_EXPANSION_WARNING: return FROM_UI_DIPLO_EVENT_EXPANSION_WARNING_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_YOU_PLOT_BUYING_SERIOUS_WARNING: return FROM_UI_DIPLO_EVENT_PLOT_BUYING_SERIOUS_WARNING_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_YOU_PLOT_BUYING_WARNING: return FROM_UI_DIPLO_EVENT_PLOT_BUYING_WARNING_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_WORK_WITH_US: return FROM_UI_DIPLO_EVENT_WORK_WITH_US_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_COOP_WAR: return FROM_UI_DIPLO_EVENT_COOP_WAR_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_COOP_WAR_TIME: return FROM_UI_DIPLO_EVENT_COOP_WAR_NOW_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_PLAN_RESEARCH_AGREEMENT: return FROM_UI_DIPLO_EVENT_PLAN_RA_RESPONSE;
	case DIPLO_UI_STATE_DISCUSS_AI_REQUEST_DENOUNCE: return FROM_UI_DIPLO_EVENT_AI_REQUEST_DENOUNCE_RESPONSE;
	case DIPLO_UI_STATE_CAUGHT_YOUR_SPY: return FROM_UI_DIPLO_EVENT_CAUGHT_YOUR_SPY_RESPONSE;
	case DIPLO_UI_STATE_KILLED_MY_SPY: return FROM_UI_DIPLO_EVENT_KILLED_MY_SPY_RESPONSE;
	case DIPLO_UI_STATE_STOP_CONVERSIONS: return FROM_UI_DIPLO_EVENT_STOP_CONVERSIONS;
	case DIPLO_UI_STATE_STOP_DIGGING: return FROM_UI_DIPLO_EVENT_STOP_DIGGING;
	default: return -1;
	}
}

}  // namespace

// SIMPLIFIED(SIMP-003): the AI's proposals to the human are declined automatically (no leader screen in B1). In a network game the
// gamecore queues the requests per human and sends one only while that human is the active player with an active turn (CvDiplomacyRequests::Update,
// :131-146), so RtSession::NetworkFrame runs it for each human under ActiveAs and calls Process(human); and it makes AI trade offers to the active player
// only (:245-254) and the server seat is active in the AI phase, so TRADE_AI_MAKES_OFFER does not occur there: the one visible difference from SIMP-003 is
// that the journal has no "declined" lines for offers. The answer is the one the
// human's "no" sends: TRADE_AI_MAKES_OFFER closes the trade screen (DoTradeScreenClosed), a demand / request goes through
// FROM_UI_DIPLO_EVENT_*_HUMAN_REFUSAL, a discussion with buttons gets its *_RESPONSE event with the refusing button 2, anything else is a
// statement that is just dismissed. Each one is an RtDiploEvent.
void RtDiplo::Answer(const Message& m, RtDiploEvent& e)
{
	CvGame& game = GC.getGame();
	const PlayerTypes from = (PlayerTypes)m.from;
	CvPlayer& ai = GET_PLAYER(from);
	switch (m.state)
	{
	case DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER:
		// The screen closes: the AI's offer is gone; the research agreement it was planning is cancelled (CvDealAI.cpp:4060).
		if (ai.isAlive() && !ai.isHuman()) ai.GetDealAI()->DoTradeScreenClosed(true);
		e.answer = RTDIPLO_DECLINED_OFFER;
		break;
	case DIPLO_UI_STATE_TRADE_AI_MAKES_DEMAND:
		e.answer = RTDIPLO_REFUSED_DEMAND;
		e.event = FROM_UI_DIPLO_EVENT_DEMAND_HUMAN_REFUSAL;
		game.DoFromUIDiploEvent(FROM_UI_DIPLO_EVENT_DEMAND_HUMAN_REFUSAL, from, 0, 0);
		if (ai.isAlive() && !ai.isHuman()) ai.GetDealAI()->DoTradeScreenClosed(false);
		break;
	case DIPLO_UI_STATE_TRADE_AI_MAKES_REQUEST:
	{
		// TradeLogic.lua passes the value of the refused deal for the AI (Player:GetDealMyValue(scratch deal)).
		int mine = 0, theirs = 0;
		if (m_scratch && ai.isAlive() && !ai.isHuman()) ai.GetDealAI()->GetDealValue(m_scratch, mine, theirs, false);
		e.answer = RTDIPLO_REFUSED_REQUEST;
		e.event = FROM_UI_DIPLO_EVENT_REQUEST_HUMAN_REFUSAL;
		e.arg1 = mine;
		game.DoFromUIDiploEvent(FROM_UI_DIPLO_EVENT_REQUEST_HUMAN_REFUSAL, from, mine, 0);
		if (ai.isAlive() && !ai.isHuman()) ai.GetDealAI()->DoTradeScreenClosed(false);
		break;
	}
	default:
	{
		const int ev = ResponseEvent(m.state);
		if (ev >= 0)
		{
			e.answer = RTDIPLO_REPLIED_NO;
			e.event = ev;
			e.arg1 = 2;
			e.arg2 = m.data;
			game.DoFromUIDiploEvent((FromUIDiploEventTypes)ev, from, 2, m.data);
		}
		else
			e.answer = RTDIPLO_DISMISSED;
		break;
	}
	}
}

int RtDiplo::Process() { return Process(NO_PLAYER); }

// `to` NO_PLAYER: every queued message (single-player game: the one human is the active player).
int RtDiplo::Process(PlayerTypes to)
{
	if (m_processing || m_pending.empty()) return 0;
	m_processing = true;
	int n = 0;
	// Only what is queued now; the DLL's follow-up messages come with the loopback delivery of the commands sent here.
	std::deque<Message> work, keep;
	for (std::deque<Message>::const_iterator it = m_pending.begin(); it != m_pending.end(); ++it)
		((to == NO_PLAYER || it->to == (int)to) ? work : keep).push_back(*it);
	m_pending.swap(keep);
	for (std::deque<Message>::const_iterator it = work.begin(); it != work.end(); ++it)
	{
		const PlayerTypes me = (PlayerTypes)it->to;
		// The answer is the human's own action: in a network game as that human, also for a message left over from a frame where his turn was not active.
		ActiveAs as(me, CvPreGame::isNetworkMultiplayerGame() && me >= 0 && me < MAX_PLAYERS);
		RtDiploEvent e;
		e.turn = it->turn;
		e.from = it->from;
		e.to = it->to;
		e.state = it->state;
		e.anim = it->anim;
		e.data = it->data;
		e.text = it->text;
		CvDiplomacyRequests* reqs = (me >= 0 && me < MAX_PLAYERS) ? GET_PLAYER(me).GetDiplomacyRequests() : NULL;
		e.request = reqs && reqs->HasActiveRequestFrom((PlayerTypes)it->from);
		Answer(*it, e);
		// UI.RequestLeaveLeader -> ActiveDiplomacyRequestComplete on the local player: the AI's turn goes on (CvGame.cpp:1752).
		if (reqs) reqs->ActiveRequestComplete();
		++m_total;
		++m_byState[it->state];
		++m_answers[e.answer];
		++n;
		Log(LOG_INFO, "diplo: %s", Describe(e).c_str());
		m_events.push_back(e);
	}
	m_processing = false;
	return n;
}

std::vector<RtDiploEvent> RtDiplo::TakeEvents()
{
	std::vector<RtDiploEvent> out;
	out.swap(m_events);
	return out;
}

int RtDiplo::Count(int state) const
{
	std::map<int, int>::const_iterator it = m_byState.find(state);
	return it == m_byState.end() ? 0 : it->second;
}

const char* RtDiplo::StateName(int state)
{
	static const char* const names[] = {
		"DIPLO_UI_STATE_DEFAULT_ROOT", "DIPLO_UI_STATE_WAR_DECLARED_BY_HUMAN", "DIPLO_UI_STATE_PEACE_MADE_BY_HUMAN", "DIPLO_UI_STATE_TRADE",
		"DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER", "DIPLO_UI_STATE_TRADE_AI_ACCEPTS_OFFER", "DIPLO_UI_STATE_TRADE_AI_REJECTS_OFFER",
		"DIPLO_UI_STATE_TRADE_AI_MAKES_DEMAND", "DIPLO_UI_STATE_TRADE_AI_MAKES_REQUEST", "DIPLO_UI_STATE_TRADE_HUMAN_OFFERS_CONCESSIONS",
		"DIPLO_UI_STATE_HUMAN_DEMAND", "DIPLO_UI_STATE_BLANK_DISCUSSION_RETURN_TO_ROOT", "DIPLO_UI_STATE_BLANK_DISCUSSION",
		"DIPLO_UI_STATE_BLANK_DISCUSSION_MEAN_HUMAN", "DIPLO_UI_STATE_BLANK_DISCUSSION_MEAN_AI", "DIPLO_UI_STATE_AI_DECLARED_WAR",
		"DIPLO_UI_STATE_DISCUSS_HUMAN_INVOKED", "DIPLO_UI_STATE_DISCUSS_AGGRESSIVE_MILITARY_WARNING", "DIPLO_UI_STATE_DISCUSS_I_ATTACKED_YOUR_MINOR_CIV",
		"DIPLO_UI_STATE_DISCUSS_I_BULLIED_YOUR_MINOR_CIV", "DIPLO_UI_STATE_DISCUSS_YOU_ATTACKED_MINOR_CIV", "DIPLO_UI_STATE_DISCUSS_YOU_KILLED_MINOR_CIV",
		"DIPLO_UI_STATE_DISCUSS_YOU_BULLIED_MINOR_CIV", "DIPLO_UI_STATE_DISCUSS_PROTECT_MINOR_CIV", "DIPLO_UI_STATE_DISCUSS_YOU_EXPANSION_SERIOUS_WARNING",
		"DIPLO_UI_STATE_DISCUSS_YOU_EXPANSION_WARNING", "DIPLO_UI_STATE_DISCUSS_YOU_PLOT_BUYING_SERIOUS_WARNING", "DIPLO_UI_STATE_DISCUSS_YOU_PLOT_BUYING_WARNING",
		"DIPLO_UI_STATE_DISCUSS_WORK_WITH_US", "DIPLO_UI_STATE_DISCUSS_WORK_AGAINST_SOMEONE", "DIPLO_UI_STATE_DISCUSS_COOP_WAR",
		"DIPLO_UI_STATE_DISCUSS_COOP_WAR_TIME", "DIPLO_UI_STATE_DISCUSS_PLAN_RESEARCH_AGREEMENT", "DIPLO_UI_STATE_DISCUSS_AI_REQUEST_DENOUNCE",
		"DIPLO_UI_STATE_CAUGHT_YOUR_SPY", "DIPLO_UI_STATE_KILLED_YOUR_SPY", "DIPLO_UI_STATE_KILLED_MY_SPY", "DIPLO_UI_STATE_CONFRONT_YOU_KILLED_MY_SPY",
		"DIPLO_UI_STATE_STOP_CONVERSIONS", "DIPLO_UI_STATE_STOP_DIGGING"};
	return (state >= 0 && state < NUM_DIPLO_UI_STATES && state < (int)(sizeof(names) / sizeof(names[0]))) ? names[state] : "DIPLO_UI_STATE_?";
}

const char* RtDiplo::AnswerName(int answer)
{
	static const char* const names[] = {"declined_offer", "refused_demand", "refused_request", "replied_no", "dismissed"};
	return (answer >= 0 && answer < NUM_RTDIPLO_ANSWERS) ? names[answer] : "?";
}

std::string RtDiplo::Describe(const RtDiploEvent& e)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "turn %d: player %d -> %d %s (%s), %s", e.turn, e.from, e.to, StateName(e.state), e.request ? "request" : "message", AnswerName(e.answer));
	std::string s = buf;
	if (e.event >= 0)
	{
		snprintf(buf, sizeof(buf), " [FromUIDiploEvent %d, args %d %d]", e.event, e.arg1, e.arg2);
		s += buf;
	}
	if (!e.text.empty()) s += ": \"" + e.text + "\"";
	return s;
}

}  // namespace lekmod_portable
