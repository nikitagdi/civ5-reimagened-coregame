// rt_events: the event queue of the runtime (task F7).
// The DLL-side hooks (RtUserInterface: notifications, popups, turn start; RtEngine: war state; RtScriptSystem: Lua errors; RtSession::Pump:
// turn changes and diplomacy auto-declines) push GC_EVENT_* records here in the order they happened; gc_pump (abi/gc_abi.cpp) hands them to
// gc_host.game_event after the pump call returned, so the text pointers are valid during the callback and the DLL is never re-entered.
// A host that never drains (the scenarios) keeps the newest kMaxEvents.
#pragma once

#include <deque>
#include <stdint.h>
#include <string>

#include "gamecore_abi.h"

namespace lekmod_portable {

struct RtEvent
{
	enum { kMaxArgs = 8 };  // v2: NOTIFICATION_ADDED has 7 args
	int kind;                  // GC_EVENT_*
	int argc;
	int args[kMaxArgs];
	bool hasText, hasText2;    // false -> NULL in gc_event
	std::string text, text2;
	RtEvent() : kind(0), argc(0), hasText(false), hasText2(false) { for (int i = 0; i < kMaxArgs; ++i) args[i] = 0; }
};

class RtEventQueue
{
public:
	static const size_t kMaxEvents = 65536;

	// Builds an event: Add(kind, argc, a0, a1, ...) is spelled out by the callers through Make + Push.
	static RtEvent Make(int kind, int argc, int a0 = 0, int a1 = 0, int a2 = 0, int a3 = 0, int a4 = 0, int a5 = 0, int a6 = 0, int a7 = 0)
	{
		RtEvent e;
		e.kind = kind;
		e.argc = argc;
		e.args[0] = a0; e.args[1] = a1; e.args[2] = a2; e.args[3] = a3; e.args[4] = a4; e.args[5] = a5; e.args[6] = a6; e.args[7] = a7;
		return e;
	}
	void Push(const RtEvent& e)
	{
		if (m_q.size() >= kMaxEvents) m_q.pop_front();
		m_q.push_back(e);
	}
	void Push(int kind, int argc, int a0 = 0, int a1 = 0, int a2 = 0, int a3 = 0, int a4 = 0, int a5 = 0, const char* text = 0, const char* text2 = 0, int a6 = 0)
	{
		RtEvent e = Make(kind, argc, a0, a1, a2, a3, a4, a5, a6);
		if (text) { e.hasText = true; e.text = text; }
		if (text2) { e.hasText2 = true; e.text2 = text2; }
		Push(e);
	}
	bool Pop(RtEvent& out)
	{
		if (m_q.empty()) return false;
		out = m_q.front();
		m_q.pop_front();
		return true;
	}
	size_t Size() const { return m_q.size(); }
	void Clear() { m_q.clear(); }

private:
	std::deque<RtEvent> m_q;
};

}  // namespace lekmod_portable
