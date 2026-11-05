// rt_diplo: auto-decline of AI diplomacy towards the human (task F6, SIMP-003).
// The gamecore shows the AI's proposals and messages to the local human through gDLL->GameplayDiplomacyAILeaderMessage
// (CvDiplomacyRequests::Send for AI-initiated requests, CvDiplomacyAI::DoFromUIDiploEvent for the replies to the human's own events).
// CvDiplomacyRequests::Send also sets m_bRequestActive, which keeps the AI player turn-active (CvGame::CheckPlayerTurnDeactivate) until
// the leader screen closes (UI.RequestLeaveLeader -> ICvPlayer1::ActiveDiplomacyRequestComplete on the local player). There is no screen:
//  - RtEngine::GameplayDiplomacyAILeaderMessage only queues the message (RtDiplo::Push); the DLL's call is never answered inside it.
//  - RtSession::Pump calls RtDiplo::Process after ICvGame1::Update: every queued message is answered the way the human's "no" goes through
//    the gamecore (as civhost's lockstep bot, headless/tools/civsim/lockstep/bot/bot.lua leaderMessage) and the request is completed.
//  - isDiploActive / isDiploOrPopupWaiting are true only while a message is queued and not yet answered.
// Every message becomes an RtDiploEvent (RtSession::Diplo().TakeEvents(); the ABI layer F7 emits them) and is counted per state.
// Ported from headless/host/src/engine/HstUserInterface.h (HostLeaderMessage) and headless/tools/civsim/lockstep/bot/bot.lua (leaderMessage).
#pragma once

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../host/PortableHost.h"

class CvDeal;

namespace lekmod_portable {

// What the host did with a leader message.
enum RtDiploAnswer
{
	RTDIPLO_DECLINED_OFFER,    // TRADE_AI_MAKES_OFFER: the trade screen closed without a proposal (CvDealAI::DoTradeScreenClosed(true))
	RTDIPLO_REFUSED_DEMAND,    // TRADE_AI_MAKES_DEMAND: FROM_UI_DIPLO_EVENT_DEMAND_HUMAN_REFUSAL
	RTDIPLO_REFUSED_REQUEST,   // TRADE_AI_MAKES_REQUEST: FROM_UI_DIPLO_EVENT_REQUEST_HUMAN_REFUSAL (arg1 = the deal's value for the AI)
	RTDIPLO_REPLIED_NO,        // a discussion with buttons (AI warning / question): the "no" button (2) of its *_RESPONSE event
	RTDIPLO_DISMISSED,         // a statement without choices (AI answer, war declaration, ...): the screen just closes
	NUM_RTDIPLO_ANSWERS
};

struct RtDiploEvent
{
	int turn;              // game turn when the message arrived
	int from;              // the AI player
	int to;                // the human (active player)
	int state;             // DiploUIStateTypes
	int anim;              // LeaderheadAnimationTypes
	int data;              // iData1 of the message
	std::string text;      // the leader's text as the DLL built it
	RtDiploAnswer answer;
	int event;             // FromUIDiploEventTypes sent, -1 when none
	int arg1, arg2;        // its arguments
	bool request;          // the message was an AI request (CvDiplomacyRequests::Send set m_bRequestActive), not a reply
	RtDiploEvent() : turn(-1), from(-1), to(-1), state(-1), anim(-1), data(-1), answer(RTDIPLO_DISMISSED), event(-1), arg1(0), arg2(0), request(false) {}
};

class RtDiplo
{
public:
	RtDiplo();
	~RtDiplo();

	// ---- DLL side (RtEngine / RtUserInterface forward here) ----
	void Push(PlayerTypes from, DiploUIStateTypes state, const char* text, LeaderheadAnimationTypes anim, int data);
	// CvDiplomacyRequests::SetScratchDeal(ICvDeal1*): the AI's offer / request deal, kept (a copy) while the screen would be open.
	void SetScratchDeal(ICvDeal1* deal);
	// True while a message is queued and not yet answered.
	bool Active() const { return !m_pending.empty(); }
	int Pending() const { return (int)m_pending.size(); }

	// ---- host side ----
	// Answers every queued message (the answers' own follow-up messages arrive through the loopback in the next Pump). Returns how many were
	// answered. Must be called outside the DLL's callback: RtSession::Pump after ICvGame1::Update.
	int Process();
	// Only the messages sent to human `to` (a network game: the DLL sends one while its human is the active player under ActiveAs, and the follow-ups of an answer
	// are sent while the same human is active, so the target is the active player at Push); the others stay queued for their own human.
	int Process(PlayerTypes to);
	// Events not yet taken (oldest first); TakeEvents hands them over and clears the queue.
	const std::vector<RtDiploEvent>& Events() const { return m_events; }
	std::vector<RtDiploEvent> TakeEvents();
	// Totals over the game: all messages, and per DiploUIStateTypes / per answer.
	int Total() const { return m_total; }
	int Count(int state) const;
	int AnswerCount(RtDiploAnswer answer) const { return (answer >= 0 && answer < NUM_RTDIPLO_ANSWERS) ? m_answers[answer] : 0; }
	// Messages that were AI requests and got a refusal (everything but RTDIPLO_DISMISSED).
	int Declined() const { return m_total - m_answers[RTDIPLO_DISMISSED]; }
	const std::map<int, int>& CountsByState() const { return m_byState; }
	// "DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER"; "DIPLO_UI_STATE_?" for an unknown value.
	static const char* StateName(int state);
	static const char* AnswerName(int answer);
	// Localized text of the event ("<Leader> proposes ... (declined automatically)" style one-liner for the log).
	static std::string Describe(const RtDiploEvent& e);

private:
	friend struct RtSaveAccess;  // rt_save.cpp (G4a): the runtime state a save carries
	RtDiplo(const RtDiplo&);
	RtDiplo& operator=(const RtDiplo&);

	struct Message
	{
		int turn, from, to, state, anim, data;  // to: the active player when the DLL sent it (the human, under ActiveAs)
		std::string text;
	};
	void Answer(const Message& m, RtDiploEvent& e);

	std::deque<Message> m_pending;
	std::vector<RtDiploEvent> m_events;
	std::map<int, int> m_byState;
	int m_answers[NUM_RTDIPLO_ANSWERS];
	int m_total;
	bool m_processing;
	CvDeal* m_scratch;   // copy of the last scratch deal (NULL when none)
};

}  // namespace lekmod_portable
