// rt_save (task G4a): RtSession::SaveGame / CheckSave / LoadGame and the runtime state of a save. Format: rt_save.h.
// SIMPLIFIED(SIMP-019): own GCS1 container (sections, xxh64, build id) instead of the exe's .Civ5Save header + zlib stream.
// Ported from headless/host/src/control/HstSession.cpp (SaveGameImpl, LoadGameImpl, HstReadSection) and net/HstLoopback.cpp
// (WriteHostState / ReadHostState).
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CvDllContext.h"
#include "FireWorks/FMemoryStream.h"
#include "ICvDLLDatabaseUtility.h"
#include "rt_active.h"
#include "rt_autoplay.h"
#include "rt_lua.h"
#include "rt_save.h"
#include "rt_session.h"
#include "lekmod_portable/host_log.h"

#ifndef LEKMOD_PORTABLE_BUILD_TAG
#define LEKMOD_PORTABLE_BUILD_TAG "dev"
#endif

namespace lekmod_portable {

namespace {

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

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

std::string ContextName(const std::string& path)
{
	size_t slash = path.find_last_of("\\/");
	std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
	if (s.size() > 4 && (s.compare(s.size() - 4, 4, ".lua") == 0 || s.compare(s.size() - 4, 4, ".LUA") == 0)) s.erase(s.size() - 4);
	return s;
}

// The runtime section has its own version: a change of its layout bumps it. Version 3 saves the per-human auto-choice counters (RtAutoChoice::m_by) instead of the
// three legacy fields; version 2 is still read (kGcsFormatVersion is the container's and stays).
const uint32_t kRuntimeVersion = 3;
const uint32_t kRuntimeVersionOldest = 2;
uint32_t g_loadingRuntimeVersion = kRuntimeVersion;  // the version of the runtime section being read (set by part 1, used by part 2 of the same load)
const uint32_t kAutoplayVersion = 1;

struct Section
{
	std::string name;
	const uint8_t* data;
	size_t size;
};

// The section table of a GCS1 buffer; false + error for anything malformed. Does not check the build id.
bool ParseContainer(const uint8_t* p, size_t n, std::vector<Section>& out, std::string& error)
{
	out.clear();
	if (!p || n < 4 || memcmp(p, "GCS1", 4) != 0)
	{
		error = Format("not a GCS1 save (bad magic, %u bytes)", (unsigned)n);
		return false;
	}
	if (n < kGcsFixedHeader)
	{
		error = Format("truncated GCS1 save: %u bytes, the fixed header alone is %u", (unsigned)n, (unsigned)kGcsFixedHeader);
		return false;
	}
	RtByteReader h(p, kGcsFixedHeader);
	h.Take(4);
	const uint32_t version = h.U32();
	const uint32_t headerSize = h.U32();
	const uint32_t count = h.U32();
	const uint64_t bodyLen = h.U64();
	const uint64_t bodyHash = h.U64();
	if (version != kGcsFormatVersion)
	{
		error = Format("save format version %u is not supported (this gamecore reads GCS1 version %u)", (unsigned)version, (unsigned)kGcsFormatVersion);
		return false;
	}
	if (headerSize != kGcsFixedHeader)
	{
		error = Format("bad GCS1 header size %u", (unsigned)headerSize);
		return false;
	}
	if (bodyLen != (uint64_t)(n - kGcsFixedHeader))
	{
		error = Format("truncated or padded GCS1 save: %u body bytes, the header says %llu", (unsigned)(n - kGcsFixedHeader), (unsigned long long)bodyLen);
		return false;
	}
	const uint64_t actual = Xxh64(p + kGcsFixedHeader, n - kGcsFixedHeader, 0);
	if (actual != bodyHash)
	{
		error = Format("corrupt GCS1 save: body checksum %016llx, the header says %016llx", (unsigned long long)actual, (unsigned long long)bodyHash);
		return false;
	}
	RtByteReader r(p + kGcsFixedHeader, n - kGcsFixedHeader);
	if (count > 1024)
	{
		error = Format("bad GCS1 section count %u", (unsigned)count);
		return false;
	}
	for (uint32_t i = 0; i < count && r.Ok(); ++i)
	{
		Section s;
		const uint32_t len = r.U16();
		const uint8_t* name = r.Take(len);
		s.size = r.U32();
		s.data = r.Take(s.size);
		if (!r.Ok() || !name) break;
		s.name.assign((const char*)name, len);
		out.push_back(s);
	}
	if (!r.Ok() || !r.AtEnd())
	{
		error = "malformed GCS1 section table";
		return false;
	}
	return true;
}

const Section* FindSection(const std::vector<Section>& sections, const std::string& name)
{
	for (size_t i = 0; i < sections.size(); ++i)
		if (sections[i].name == name) return &sections[i];
	return NULL;
}

std::string HeaderValue(const std::string& text, const char* key)
{
	const std::string k = std::string(key) + "=";
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		if (text.compare(pos, k.size(), k) == 0) return text.substr(pos + k.size(), end - pos - k.size());
		pos = end + 1;
	}
	return std::string();
}

// The sections every save has (beyond header / setup / runtime / abi).
std::vector<std::string> DllSectionNames()
{
	std::vector<std::string> names;
	names.push_back("pregame");
	names.push_back("game");
	names.push_back("map");
	for (int i = 0; i < MAX_TEAMS; ++i) names.push_back(Format("team%02d", i));
	for (int i = 0; i < MAX_PLAYERS; ++i) names.push_back(Format("player%02d", i));
	names.push_back("supporting");
	return names;
}

void AddStream(std::vector<std::pair<std::string, std::vector<uint8_t> > >& sections, const std::string& name, const FMemoryStream& stream)
{
	const uint8_t* b = (const uint8_t*)stream.GetBuf();
	sections.push_back(std::make_pair(name, std::vector<uint8_t>(b, b + stream.GetEOF())));
}

}  // namespace

// ---- runtime state --------------------------------------------------------------------------------------------------------------

struct RtSaveAccess
{
	// Loopback and engine (HstEngineUtility::WriteHostState plus the virtual clock and the war-state memory).
	static void WriteEngine(const RtEngine& e, RtByteWriter& w)
	{
		w.U32(e.m_virtualMs);
		w.I32(e.m_netDelivered);
		for (int i = 0; i < MAX_PLAYERS; ++i) w.U8((e.m_turnCompleteSent[i] ? 1 : 0) | (e.m_turnCompleteReceived[i] ? 2 : 0));
		w.U8((e.m_aiCivsProcessedSent ? 1 : 0) | (e.m_aiCivsProcessedReceived ? 2 : 0));
		for (int a = 0; a < MAX_TEAMS; ++a)
			for (int b = a + 1; b < MAX_TEAMS; ++b) w.Bool(e.m_war[a][b]);
		w.U32((uint32_t)e.m_netQueue.size());
		for (size_t i = 0; i < e.m_netQueue.size(); ++i)
		{
			const RtNetMessage& m = e.m_netQueue[i];
			w.I32(m.eId);
			w.I32((int)m.ePlayer);
			for (int j = 0; j < 9; ++j) w.I32(m.aiArg[j]);
			w.Str(m.strText);
		}
		// Network turn model (N3.13): what the exe's handshake knows between two frames. At a safe point the barrier of the last turn is closed
		// and the all-AI-processed stamp of this turn is set; without them a loaded game does not see the AI civs as done.
		for (int i = 0; i < MAX_PLAYERS; ++i) w.I32(e.m_turnAllCompleteTurn[i]);
		w.I32(e.m_allAIProcessedTurn);
		w.I32(e.m_barrierTurn);
		w.Bool(e.m_sentTurnAllComplete);
		for (int i = 0; i < MAX_PLAYERS; ++i) w.U8((e.m_turnUnreadySent[i] ? 1 : 0) | (e.m_kicked[i] ? 2 : 0));
	}
	static bool ReadEngine(RtEngine& e, RtByteReader& r)
	{
		e.m_virtualMs = r.U32();
		e.m_netDelivered = r.I32();
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			const uint32_t f = r.U8();
			e.m_turnCompleteSent[i] = (f & 1) != 0;
			e.m_turnCompleteReceived[i] = (f & 2) != 0;
		}
		const uint32_t f = r.U8();
		e.m_aiCivsProcessedSent = (f & 1) != 0;
		e.m_aiCivsProcessedReceived = (f & 2) != 0;
		for (int a = 0; a < MAX_TEAMS; ++a)
			for (int b = a + 1; b < MAX_TEAMS; ++b) e.m_war[a][b] = r.Bool();
		const uint32_t n = r.Count(44);
		e.m_netQueue.clear();
		e.m_netDelivering.clear();
		for (uint32_t i = 0; i < n && r.Ok(); ++i)
		{
			RtNetMessage m;
			m.eId = r.I32();
			m.ePlayer = (PlayerTypes)r.I32();
			for (int j = 0; j < 9; ++j) m.aiArg[j] = r.I32();
			m.strText = r.Str(65536);
			e.m_netQueue.push_back(m);
		}
		for (int i = 0; i < MAX_PLAYERS; ++i) e.m_turnAllCompleteTurn[i] = r.I32();
		e.m_allAIProcessedTurn = r.I32();
		e.m_barrierTurn = r.I32();
		e.m_sentTurnAllComplete = r.Bool();
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			const uint32_t f = r.U8();
			e.m_turnUnreadySent[i] = (f & 1) != 0;
			e.m_kicked[i] = (f & 2) != 0;
		}
		return r.Ok();
	}

	static void WriteUi(const RtUserInterface& u, RtByteWriter& w)
	{
		w.Bool(u.m_canEndTurn);
		for (int i = 0; i < MAX_PLAYERS; ++i) w.Bool(u.m_canEndTurnBy[i]);
		w.Bool(u.m_hasMovedUnit);
		w.Bool(u.m_policyNotificationSeen);
		w.U32((uint32_t)u.m_notifications.size());
		for (std::map<int, RtNotification>::const_iterator it = u.m_notifications.begin(); it != u.m_notifications.end(); ++it)
		{
			const RtNotification& n = it->second;
			w.I32(n.id);
			w.I32(n.type);
			w.I32(n.gameData);
			w.I32(n.extraGameData);
			w.I32(n.player);
			w.I32(n.x);
			w.I32(n.y);
			w.I32(n.turn);
			w.Str(n.message);
			w.Str(n.summary);
		}
		w.U32((uint32_t)u.m_pendingGoodies.size());
		for (size_t i = 0; i < u.m_pendingGoodies.size(); ++i)
		{
			w.I32(u.m_pendingGoodies[i].player);
			w.I32(u.m_pendingGoodies[i].unit);
			w.I32(u.m_pendingGoodies[i].x);
			w.I32(u.m_pendingGoodies[i].y);
		}
	}
	static bool ReadUi(RtUserInterface& u, RtByteReader& r)
	{
		u.m_canEndTurn = r.Bool();
		for (int i = 0; i < MAX_PLAYERS; ++i) u.m_canEndTurnBy[i] = r.Bool();
		u.m_hasMovedUnit = r.Bool();
		u.m_policyNotificationSeen = r.Bool();
		u.m_notifications.clear();
		const uint32_t n = r.Count(40);
		for (uint32_t i = 0; i < n && r.Ok(); ++i)
		{
			RtNotification x;
			x.id = r.I32();
			x.type = r.I32();
			x.gameData = r.I32();
			x.extraGameData = r.I32();
			x.player = r.I32();
			x.x = r.I32();
			x.y = r.I32();
			x.turn = r.I32();
			x.message = r.Str();
			x.summary = r.Str();
			u.m_notifications[x.id] = x;
		}
		u.m_pendingGoodies.clear();
		const uint32_t g = r.Count(16);
		for (uint32_t i = 0; i < g && r.Ok(); ++i)
		{
			RtUserInterface::PendingGoody pg;
			pg.player = r.I32();
			pg.unit = r.I32();
			pg.x = r.I32();
			pg.y = r.I32();
			u.m_pendingGoodies.push_back(pg);
		}
		return r.Ok();
	}

	// The open messages and the counters; the event log (Events()) starts empty after a load.
	static void WriteDiplo(const RtDiplo& d, RtByteWriter& w)
	{
		w.U32((uint32_t)d.m_pending.size());
		for (std::deque<RtDiplo::Message>::const_iterator it = d.m_pending.begin(); it != d.m_pending.end(); ++it)
		{
			w.I32(it->turn);
			w.I32(it->from);
			w.I32(it->state);
			w.I32(it->anim);
			w.I32(it->data);
			w.Str(it->text);
		}
		w.U32((uint32_t)d.m_byState.size());
		for (std::map<int, int>::const_iterator it = d.m_byState.begin(); it != d.m_byState.end(); ++it)
		{
			w.I32(it->first);
			w.I32(it->second);
		}
		for (int i = 0; i < NUM_RTDIPLO_ANSWERS; ++i) w.I32(d.m_answers[i]);
		w.I32(d.m_total);
	}
	static bool ReadDiplo(RtDiplo& d, RtByteReader& r)
	{
		d.m_pending.clear();
		const uint32_t n = r.Count(24);
		for (uint32_t i = 0; i < n && r.Ok(); ++i)
		{
			RtDiplo::Message m;
			m.turn = r.I32();
			m.from = r.I32();
			m.state = r.I32();
			m.anim = r.I32();
			m.data = r.I32();
			m.text = r.Str();
			d.m_pending.push_back(m);
		}
		d.m_byState.clear();
		const uint32_t s = r.Count(8);
		for (uint32_t i = 0; i < s && r.Ok(); ++i)
		{
			const int k = r.I32();
			d.m_byState[k] = r.I32();
		}
		for (int i = 0; i < NUM_RTDIPLO_ANSWERS; ++i) d.m_answers[i] = r.I32();
		d.m_total = r.I32();
		d.m_events.clear();
		return r.Ok();
	}

	static void WriteFlags(const RtAutoChoices& f, RtByteWriter& w)
	{
		const int v[11] = {f.research, f.policy, f.ideology, f.religion, f.promotion, f.great_person, f.captured_city, f.league, f.archaeology, f.goody_hut, f.steal_tech};
		for (int i = 0; i < 11; ++i) w.I32(v[i]);
	}
	static void ReadFlags(RtAutoChoices& f, RtByteReader& r)
	{
		int* v[11] = {&f.research, &f.policy, &f.ideology, &f.religion, &f.promotion, &f.great_person, &f.captured_city, &f.league, &f.archaeology, &f.goody_hut, &f.steal_tech};
		for (int i = 0; i < 11; ++i) *v[i] = r.I32();
	}
	static void WriteAutoChoice(const RtAutoChoice& a, RtByteWriter& w)
	{
		w.Bool(a.m_flagsSet);
		WriteFlags(a.m_flags, w);
		w.I32(a.m_choices);
		// Runtime version 3: the attempt counters of every human that has run (m_by, sparse: u32 count, then per entry the player, the turn, the 64 attempts, the 64 logged flags).
		uint32_t n = 0;
		for (int p = 0; p < MAX_PLAYERS; ++p)
			if (a.m_by[p].turn != -1) ++n;
		w.U32(n);
		for (int p = 0; p < MAX_PLAYERS; ++p)
		{
			const RtAutoChoice::Counters& c = a.m_by[p];
			if (c.turn == -1) continue;
			w.I32(p);
			w.I32(c.turn);
			for (int i = 0; i < 64; ++i) w.I32(c.attempts[i]);
			for (int i = 0; i < 64; ++i) w.Bool(c.gaveUpLogged[i]);
		}
	}
	static bool ReadAutoChoice(RtAutoChoice& a, RtByteReader& r, uint32_t runtimeVersion)
	{
		a.m_flagsSet = r.Bool();
		ReadFlags(a.m_flags, r);
		a.m_choices = r.I32();
		memset(a.m_by, 0, sizeof(a.m_by));
		for (int p = 0; p < MAX_PLAYERS; ++p) a.m_by[p].turn = -1;
		if (runtimeVersion < 3)
		{
			// Runtime version 2 (older saves): the three legacy fields; no decision read them, a load starts the counters of every human at 0.
			r.I32();
			for (int i = 0; i < 64; ++i) r.I32();
			for (int i = 0; i < 64; ++i) r.Bool();
		}
		else
		{
			const uint32_t n = r.Count(8);
			for (uint32_t k = 0; k < n && r.Ok(); ++k)
			{
				const int p = r.I32();
				RtAutoChoice::Counters tmp;
				tmp.turn = r.I32();
				for (int i = 0; i < 64; ++i) tmp.attempts[i] = r.I32();
				for (int i = 0; i < 64; ++i) tmp.gaveUpLogged[i] = r.Bool();
				if (!r.Ok() || p < 0 || p >= MAX_PLAYERS) return false;
				a.m_by[p] = tmp;
			}
		}
		a.m_events.clear();
		return r.Ok();
	}

	static void WriteLua(const RtScriptSystem& s, RtByteWriter& w)
	{
		w.U32(s.m_randState);
		w.U32(s.m_randCalls);
		w.F64(s.m_virtualClock);
	}
	static bool ReadLua(RtScriptSystem& s, RtByteReader& r)
	{
		s.m_randState = r.U32();
		s.m_randCalls = r.U32();
		s.m_virtualClock = r.F64();
		return r.Ok();
	}

	static void WriteAutoplay(const RtAutoplay& b, RtByteWriter& w)
	{
		w.U32(kAutoplayVersion);
		const RtAutoplayStats& s = b.m_stats;
		const int v[10] = {s.turns, s.rounds, s.commands, s.rejected, s.forced, s.founded, s.attacks, s.productions, s.maxRoundsPerTurn, s.retaken};
		for (int i = 0; i < 10; ++i) w.I32(v[i]);
		for (int i = 0; i < 48; ++i) w.I32(s.byKind[i]);
		w.Str(s.firstReject);
		w.Str(s.firstRetaken);
		w.I32(b.m_turn);
		w.I32(b.m_endedTurn);
		w.I32(b.m_roundsThisTurn);
		w.I32(b.m_idleRounds);
		w.Bool(b.m_wokeThisTurn);
		w.U32((uint32_t)b.m_ordersThisTurn.size());
		for (std::map<int, int>::const_iterator it = b.m_ordersThisTurn.begin(); it != b.m_ordersThisTurn.end(); ++it)
		{
			w.I32(it->first);
			w.I32(it->second);
		}
	}
	static bool ReadAutoplay(RtAutoplay& b, RtByteReader& r)
	{
		if (r.U32() != kAutoplayVersion) return false;
		RtAutoplayStats& s = b.m_stats;
		int* v[10] = {&s.turns, &s.rounds, &s.commands, &s.rejected, &s.forced, &s.founded, &s.attacks, &s.productions, &s.maxRoundsPerTurn, &s.retaken};
		for (int i = 0; i < 10; ++i) *v[i] = r.I32();
		for (int i = 0; i < 48; ++i) s.byKind[i] = r.I32();
		s.firstReject = r.Str();
		s.firstRetaken = r.Str();
		b.m_turn = r.I32();
		b.m_endedTurn = r.I32();
		b.m_roundsThisTurn = r.I32();
		b.m_idleRounds = r.I32();
		b.m_wokeThisTurn = r.Bool();
		b.m_ordersThisTurn.clear();
		const uint32_t n = r.Count(8);
		for (uint32_t i = 0; i < n && r.Ok(); ++i)
		{
			const int k = r.I32();
			b.m_ordersThisTurn[k] = r.I32();
		}
		return r.Ok();
	}

	// Part 1 of the runtime section (before the DLL Reads: the loopback state the DLL may ask about while it loads) and part 2 (after the
	// game start: UI, diplomacy, auto-choices, Lua and session state, which DoGameStarted and the contexts must not overwrite).
	static void WriteRuntime(const RtSession& s, RtByteWriter& w)
	{
		w.U32(kRuntimeVersion);
		WriteEngine(s.m_engine, w);
		WriteUi(s.m_ui, w);
		WriteDiplo(s.m_diplo, w);
		WriteAutoChoice(s.m_autochoice, w);
		WriteLua(*s.m_scripts, w);
		w.U32(s.m_turnSlices);
		w.I32(s.m_humanPhaseTurn);
	}
	static bool ReadRuntimeEngine(RtSession& s, RtByteReader& r)
	{
		const uint32_t version = r.U32();
		if (version < kRuntimeVersionOldest || version > kRuntimeVersion) return false;
		g_loadingRuntimeVersion = version;
		return ReadEngine(s.m_engine, r);
	}
	// The DLL's Read resets CvGame::m_lastTurnAICivsProcessed; a network save at the human phase is after the AI civs, so the turn is stamped again. The DLL stamps through
	// SetLastTurnAICivsProcessed, which also sends SendAICivsProcessed: the saved game delivered that message before the save (the loopback state read from the save
	// already holds it: m_allAIProcessedTurn, the sent flag), so the second copy is dropped. Left queued it would make the loaded game differ from the saved one: no human
	// waits while a message is queued, so the first gc_pump after the load gives no bot round (the rotation of the bots shifts by one) and no gc_save is possible.
	static void RestampAICivsProcessed(RtSession& s)
	{
		RtEngine& e = s.m_engine;
		const size_t queued = e.m_netQueue.size();
		const bool sent = e.m_aiCivsProcessedSent;
		GC.getGame().SetLastTurnAICivsProcessed();
		if (e.m_netQueue.size() > queued) e.m_netQueue.resize(queued);
		e.m_aiCivsProcessedSent = sent;
	}
	static bool ReadRuntimeRest(RtSession& s, RtByteReader& r)
	{
		if (!ReadUi(s.m_ui, r) || !ReadDiplo(s.m_diplo, r) || !ReadAutoChoice(s.m_autochoice, r, g_loadingRuntimeVersion) || !ReadLua(*s.m_scripts, r)) return false;
		s.m_turnSlices = r.U32();
		s.m_humanPhaseTurn = r.I32();
		return r.Ok() && r.AtEnd();
	}
};

void WriteAutoplayState(const RtAutoplay& bot, RtByteWriter& w) { RtSaveAccess::WriteAutoplay(bot, w); }
bool ReadAutoplayState(RtAutoplay& bot, RtByteReader& r) { return RtSaveAccess::ReadAutoplay(bot, r); }

// ---- RtSession ------------------------------------------------------------------------------------------------------------------

std::string RtSession::BuildId() const
{
	std::string version = "?";
	GUID g;
	memset(&g, 0, sizeof(g));
	if (m_ctx)
	{
		const char* v = m_ctx->GetDLLVersion();
		if (v) version = v;
		g = m_ctx->GetDLLGUID();
	}
	return Format("lekmod-portable/%s/dll %s/guid %08x-%04x-%04x-%02x%02x/data %016llx", LEKMOD_PORTABLE_BUILD_TAG, version.c_str(), (unsigned)g.Data1, (unsigned)g.Data2,
		(unsigned)g.Data3, (unsigned)g.Data4[0], (unsigned)g.Data4[1], (unsigned long long)m_dataHash);
}

// SIMPLIFIED(SIMP-026): only an unanswered human wait is a save point (no queued command, no AI turn, a living human); original: any time.
// Network game (N3.13): the human phase (the AI civs and the unit AI are done), nothing queued, nobody's unit or city busy, no leader message open. Who
// is up and who has ended the turn is saved with the game, so it is not a condition; the ABI layer adds the pump and command history (the last gc_pump ended in the human
// phase, no command since, no human in auto moves). The server seat is the active player there and is no human: the audit's F-07.
std::string RtSession::SavePointProblem()
{
	if (!m_inGame || !m_ctx) return "no game is running";
	if (GC.getGame().getGameState() == GAMESTATE_OVER) return "the game is over";
	if (m_engine.NetworkGame())
	{
		if (!HumanPhase()) return "the AI civilizations are still moving; pump until the humans are up";
		if (m_engine.NetQueueSize() > 0) return "commands are queued; pump until the humans are up";
		for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
			if (IsSeatedHuman(i) && GET_PLAYER((PlayerTypes)i).hasBusyUnitOrCity()) return Format("a unit or city of player %d is busy; pump until the humans are up", i);
		if (m_diplo.Pending() > 0) return "a leader message is open";
		return std::string();
	}
	if (!IsHumanWaiting())
	{
		const PlayerTypes active = m_engine.GetActivePlayer();
		if (active == NO_PLAYER || !GET_PLAYER(active).isHuman() || !GET_PLAYER(active).isAlive()) return "the active player is not a living human (observer games cannot be saved)";
		if (m_engine.HasSentTurnComplete()) return "the human has ended the turn; pump until the human is up again";
		if (m_engine.NetQueueSize() > 0) return "commands are queued; pump until WAITING_HUMAN";
		return "the AI turn is still running or a unit / city is busy; pump until WAITING_HUMAN";
	}
	if (m_diplo.Pending() > 0) return "a leader message is open";
	return std::string();
}

bool RtSession::SaveGame(const std::vector<uint8_t>& abiState, std::vector<uint8_t>& out, std::string& error)
{
	if (!m_inGame || !m_ctx || !m_scripts)
	{
		error = "no game is running";
		return false;
	}
	std::vector<std::pair<std::string, std::vector<uint8_t> > > sections;
	std::auto_ptr<ICvGame1> game(m_ctx->GetGame());
	{
		const std::string header = Format("format=GCS1 %u\nbuild_id=%s\ndll_version=%s\nabi=%u\nturn=%d\n", (unsigned)kGcsFormatVersion, BuildId().c_str(),
			m_ctx->GetDLLVersion() ? m_ctx->GetDLLVersion() : "?", (unsigned)GC_ABI_VERSION, game->GetGameTurn());
		sections.push_back(std::make_pair(std::string("header"), std::vector<uint8_t>(header.begin(), header.end())));
	}
	sections.push_back(std::make_pair(std::string("setup"), std::vector<uint8_t>(m_setupJson.begin(), m_setupJson.end())));
	{
		std::vector<uint8_t> rt;
		RtByteWriter w(rt);
		RtSaveAccess::WriteRuntime(*this, w);
		sections.push_back(std::make_pair(std::string("runtime"), rt));
	}
	sections.push_back(std::make_pair(std::string("abi"), abiState));
	{
		std::auto_ptr<ICvPreGame1> pre(m_ctx->GetPreGame());
		FMemoryStream s;
		pre->write(s);
		AddStream(sections, "pregame", s);
	}
	{
		FMemoryStream s;
		game->Write(s);
		AddStream(sections, "game", s);
	}
	{
		std::auto_ptr<ICvMap1> map(m_ctx->GetMap());
		FMemoryStream s;
		map->Write(s);
		AddStream(sections, "map", s);
	}
	for (int i = 0; i < MAX_TEAMS; ++i)
	{
		std::auto_ptr<ICvTeam1> team(m_ctx->GetTeam((TeamTypes)i));
		FMemoryStream s;
		if (team.get()) team->Write(s);
		AddStream(sections, Format("team%02d", i), s);
	}
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> player(game->GetPlayer((PlayerTypes)i));
		FMemoryStream s;
		if (player.get()) player->Write(s);
		AddStream(sections, Format("player%02d", i), s);
	}
	{
		FMemoryStream s;
		game->WriteSupportingClassData(s);
		AddStream(sections, "supporting", s);
	}
	{
		// B4 replay determinism (2026-10-31): the asynchronous RNG is not part of a Civ5 save, but the AI draws from it during
		// its turn (CvGame::GetDiploResponse picks the leader's line with getAsyncRandNum): a game loaded from the save started
		// the generator afresh where the running one went on (the RNG trace of turn 131 of game 936), so a restart changed the
		// lines the humans are told. Its own section: a build that does not know it ignores it, and the replica digest
		// (gc_state_digest -3) leaves it out (a replica's UI draws from it too).
		FMemoryStream s;
		GC.getASyncRand().write(s);
		AddStream(sections, "async", s);
	}

	std::vector<uint8_t> body;
	{
		size_t total = 0;
		for (size_t i = 0; i < sections.size(); ++i) total += 6 + sections[i].first.size() + sections[i].second.size();
		body.reserve(total);
		RtByteWriter w(body);
		for (size_t i = 0; i < sections.size(); ++i)
		{
			w.U16((uint32_t)sections[i].first.size());
			w.Bytes(sections[i].first.data(), sections[i].first.size());
			w.U32((uint32_t)sections[i].second.size());
			w.Bytes(sections[i].second.empty() ? NULL : &sections[i].second[0], sections[i].second.size());
		}
	}
	out.clear();
	out.reserve(kGcsFixedHeader + body.size());
	RtByteWriter w(out);
	w.Bytes("GCS1", 4);
	w.U32(kGcsFormatVersion);
	w.U32(kGcsFixedHeader);
	w.U32((uint32_t)sections.size());
	w.U64((uint64_t)body.size());
	w.U64(Xxh64(body.empty() ? "" : (const void*)&body[0], body.size(), 0));
	w.Bytes(body.empty() ? NULL : &body[0], body.size());
	return true;
}

bool RtSession::CheckSave(const uint8_t* p, size_t n, std::string& error) const
{
	std::vector<Section> sections;
	if (!ParseContainer(p, n, sections, error)) return false;
	if (FindSection(sections, "body"))
	{
		// Import of a .Civ5Save (rt_save.h): header, setup, pregame and the inflated body; no build id (the save comes from the exe).
		static const char* const kImport[] = {"header", "setup", "pregame"};
		for (size_t i = 0; i < 3; ++i)
			if (!FindSection(sections, kImport[i]))
			{
				error = Format("GCS1 import has no '%s' section", kImport[i]);
				return false;
			}
		return true;
	}
	static const char* const kOwn[] = {"header", "setup", "runtime", "abi"};
	for (size_t i = 0; i < 4; ++i)
		if (!FindSection(sections, kOwn[i]))
		{
			error = Format("GCS1 save has no '%s' section", kOwn[i]);
			return false;
		}
	const std::vector<std::string> dll = DllSectionNames();
	for (size_t i = 0; i < dll.size(); ++i)
		if (!FindSection(sections, dll[i]))
		{
			error = Format("GCS1 save has no '%s' section", dll[i].c_str());
			return false;
		}
	const Section* h = FindSection(sections, "header");
	const std::string header((const char*)h->data, h->size);
	const std::string saved = HeaderValue(header, "build_id");
	const std::string mine = BuildId();
	if (saved != mine)
	{
		error = Format("the save comes from another gamecore build (save: '%s', this gamecore: '%s')", saved.c_str(), mine.c_str());
		return false;
	}
	return true;
}

bool RtSession::LoadGame(const gc_host& host, const uint8_t* p, size_t n, std::vector<uint8_t>& abiState, LoadGameStatus& st)
{
#define LG_FAIL(...) do { st.error = Format(__VA_ARGS__); Log(LOG_ERROR, "session: load %s: %s", st.step.c_str(), st.error.c_str()); return false; } while (0)
	st = LoadGameStatus();
	st.step = "check";
	if (!m_loaded) LG_FAIL("session is not loaded");
	RtCoreScope core(m_engine);  // B4 gc-replica: the handshake a load sends is the core's own, also on a replica (gc_resync)
	if (m_newGameTried)
	{
		st.error = "a game was already started in this instance (one new game or load per instance)";
		Log(LOG_WARN, "session: load refused: %s", st.error.c_str());
		return false;
	}
	std::vector<Section> sections;
	std::string err;
	if (!CheckSave(p, n, err)) LG_FAIL("%s", err.c_str());
	m_newGameTried = true;

	// Lua master state (as S1 of a new game; no map script runs). CvGame::Read -> reset() deletes and recreates
	// Civ5SavedGameDatabase.db and writes the embedded copy back, so the Lua connection to it is closed first.
	st.step = "lua";
	if (!InitScripts(host, err)) LG_FAIL("%s", err.c_str());
	RtScriptSystem& ss = *m_scripts;
	ss.CloseSaveData();
	ss.ResetGameState();
	ParseContainer(p, n, sections, err);
	const Section* setup = FindSection(sections, "setup");
	m_setupJson.assign((const char*)setup->data, setup->size);
	if (!m_setup.Parse(m_setupJson, err)) LG_FAIL("setup of the save: %s", err.c_str());
	Log(LOG_INFO, "load: %s", m_setup.Describe().c_str());
	// A network game's loopback is the exe's turn model (RtEngine::NetworkGame) from the first read on.
	m_engine.SetNetworkGame(m_setup.Network());
	m_ui.SetPerPlayerCanEndTurn(m_setup.Network());
	// Import of a .Civ5Save (rt_save.h): the DLL Reads take the one body stream in turn, as the exe's loader does; the runtime and the ABI layer
	// start fresh (no "runtime" / "abi" section), as civhost's --import-save does.
	const Section* body = FindSection(sections, "body");
	std::auto_ptr<FMemoryStream> bodyStream(body ? new FMemoryStream((int)body->size, body->data) : NULL);
	abiState.clear();
	const Section* abi = FindSection(sections, "abi");
	if (abi) abiState.assign(abi->data, abi->data + abi->size);

	st.step = "runtime";
	const Section* rts = FindSection(sections, "runtime");
	RtByteReader rt(rts ? rts->data : NULL, rts ? rts->size : 0);
	if (rts && !RtSaveAccess::ReadRuntimeEngine(*this, rt)) LG_FAIL("runtime section: loopback state is corrupt");
	m_engine.ClearSlotConnections();  // the connection is not saved: the host's CONNECT brings every human back
	if (body) Log(LOG_INFO, "load: import of a .Civ5Save body (%u bytes, pregame %u bytes)", (unsigned)body->size, (unsigned)FindSection(sections, "pregame")->size);

	const double t0 = NowMs();
	// Exe Resync_LoadGameFromStream 0x10ae10: CacheGameDatabaseData, then Resync_LoadGameFromStreamImpl 0x2b6230.
	{
		std::auto_ptr<ICvDLLDatabaseUtility1> loader(m_ctx->GetDatabaseLoadUtility());
		loader->CacheGameDatabaseData();
	}
	std::auto_ptr<ICvPreGame1> pre(m_ctx->GetPreGame());
	std::auto_ptr<ICvGame1> game(m_ctx->GetGame());
	std::auto_ptr<ICvMap1> map(m_ctx->GetMap());
	// One DLL Read per section; a Read that does not consume its section exactly is logged (civhost HstReadSection). An import reads the
	// DLL part from the body in turn (each Read is given the rest of it).
#define LG_READ(NAME, CALL)                                                                                                         \
	do                                                                                                                              \
	{                                                                                                                               \
		if (bodyStream.get())                                                                                                       \
		{                                                                                                                           \
			FMemoryStream& stream = *bodyStream;                                                                                    \
			const unsigned before = stream.GetPosition();                                                                           \
			try                                                                                                                     \
			{                                                                                                                       \
				CALL;                                                                                                               \
			}                                                                                                                       \
			catch (const std::exception& e)                                                                                         \
			{                                                                                                                       \
				LG_FAIL("import: '%s' from body byte %u (now %u of %u): %s", std::string(NAME).c_str(), before,                    \
					(unsigned)stream.GetPosition(), (unsigned)body->size, e.what());                                                 \
			}                                                                                                                       \
			Log(LOG_DEBUG, "load: import '%s' read %u bytes", std::string(NAME).c_str(), (unsigned)(stream.GetPosition() - before)); \
			break;                                                                                                                  \
		}                                                                                                                           \
		const Section* sec = FindSection(sections, NAME);                                                                           \
		FMemoryStream stream((int)sec->size, sec->data);                                                                            \
		CALL;                                                                                                                       \
		if (stream.GetPosition() != sec->size)                                                                                      \
		{                                                                                                                           \
			++st.mismatches;                                                                                                        \
			Log(LOG_WARN, "load: '%s' read %u of %u bytes", std::string(NAME).c_str(), (unsigned)stream.GetPosition(), (unsigned)sec->size); \
		}                                                                                                                           \
	} while (0)
	st.step = "pregame";
	{
		const Section* sec = FindSection(sections, "pregame");
		FMemoryStream stream((int)sec->size, sec->data);
		try
		{
			pre->read(stream, true);
		}
		catch (const std::exception& e)
		{
			LG_FAIL("'pregame' at byte %u of %u: %s", (unsigned)stream.GetPosition(), (unsigned)sec->size, e.what());
		}
		if (stream.GetPosition() != sec->size)
		{
			++st.mismatches;
			Log(LOG_WARN, "load: 'pregame' read %u of %u bytes", (unsigned)stream.GetPosition(), (unsigned)sec->size);
		}
	}
	if (m_setup.Network())
	{
		// The server seat (the SS_OBSERVER slot) is the active player of a network game at rest; the pregame carries it, the rule's own copy is set here.
		PlayerTypes seat = NO_PLAYER;
		for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
			if (pre->slotStatus((PlayerTypes)i) == SS_OBSERVER) seat = (PlayerTypes)i;
		if (seat == NO_PLAYER) LG_FAIL("network save without a server seat (no SS_OBSERVER slot)");
		pre->setActivePlayer(seat);
		SetServerSeat(seat);
	}
	// 0x31bfe0: game, map, teams, players, RecalculateLandmasses, supporting data, Validate, LogGameState(true).
	st.step = "game";
	LG_READ("game", game->Read(stream));
	// The DLL's Read resets CvGame::m_lastTurnAICivsProcessed (for an autosave of a simultaneous game, whose AI civs run again). This save is taken at the human phase, after the AI civs: stamp the
	// turn as the exe's SendAICivsProcessed does, so that updateMoves does not run them a second time (it would consume random numbers the saved game did not); without the
	// message the stamp sends (RestampAICivsProcessed).
	if (m_setup.Network() && m_engine.AllAIProcessedTurn() == GC.getGame().getGameTurn()) RtSaveAccess::RestampAICivsProcessed(*this);
	st.step = "map";
	LG_READ("map", map->Read(stream));
	st.step = "teams";
	for (int i = 0; i < MAX_TEAMS; ++i)
	{
		std::auto_ptr<ICvTeam1> team(m_ctx->GetTeam((TeamTypes)i));
		LG_READ(Format("team%02d", i), if (team.get()) team->Read(stream));
	}
	st.step = "players";
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> player(game->GetPlayer((PlayerTypes)i));
		LG_READ(Format("player%02d", i), if (player.get()) player->Read(stream));
	}
	st.step = "postload";
	map->RecalculateLandmasses();
	LG_READ("supporting", game->ReadSupportingClassData(stream));
#undef LG_READ
	if (bodyStream.get())
	{
		// The tail of a real save is zero padding (civhost: 520..2288 bytes); anything else means a Read consumed too little.
		const unsigned pos = bodyStream->GetPosition();
		unsigned nonZero = 0;
		for (size_t i = pos; i < body->size; ++i) nonZero += body->data[i] != 0 ? 1 : 0;
		Log(LOG_INFO, "load: import: the DLL Reads consumed %u of %u body bytes; %u byte(s) left, %u of them non-zero", pos, (unsigned)body->size,
			(unsigned)(body->size - pos), nonZero);
		if (nonZero) ++st.mismatches;
	}
	map->Validate();
	game->LogGameState(true);
	// 0x2b6230 tail: re-set civilization / leader of slots 0..62 (rebinds the package keys).
	for (int i = 0; i < MAX_PLAYERS - 1; ++i)
	{
		const PlayerTypes slot = (PlayerTypes)i;
		pre->setCivilization(slot, pre->civilization(slot));
		pre->setLeaderHead(slot, pre->leaderHead(slot));
	}
	// 0x10ae10 tail: single-player loads drop the MP turn modes; then (game started) GAME_LOADED, InitScoreCalculation,
	// SetFinalInitialized(true).
	if (pre->isHotSeat() || pre->gameType() == GAME_SINGLE_PLAYER)
	{
		pre->SetGameOption("GAMEOPTION_SIMULTANEOUS_TURNS", 0);
		pre->SetGameOption("GAMEOPTION_DYNAMIC_TURNS", 0);
		pre->SetGameOption("GAMEOPTION_PITBOSS", 0);
	}
	if (pre->gameStarted())
	{
		pre->setGameStartType(GAME_LOADED);
		game->InitScoreCalculation();
		game->SetFinalInitialized(true);
	}
	// Not serialized: the tactical analysis map (L5b); civhost GUESS: the exe re-inits it during LoadGameState.
	game->InitTacticalAnalysisMap(map->NumPlots());
	st.msRead = NowMs() - t0;
	if (st.mismatches) Log(LOG_WARN, "load: %d DLL Read call(s) did not consume their section exactly", st.mismatches);

	// L7-L9 as after a new game (civhost StartGame(NULL, true)).
	st.step = "contexts";
	const double t1 = NowMs();
	const int errorsBefore = ss.GetErrorCount();
	const std::vector<std::string>& contexts = m_vfs.Contexts();
	for (size_t i = 0; i < contexts.size(); ++i) ss.LoadContext(ContextName(contexts[i]).c_str());
	st.step = "start";
	NewGameStatus ng;
	StartGame(ng);
	st.msStart = NowMs() - t1;
	st.luaErrors = ss.GetErrorCount() - errorsBefore;

	// The runtime's own state last: DoGameStarted and the contexts must not overwrite what the saved game had.
	if (rts && !RtSaveAccess::ReadRuntimeRest(*this, rt)) LG_FAIL("runtime section is corrupt");
	// The asynchronous RNG as the saved game left it (SaveGame); a save without the section keeps the generator as it is.
	if (const Section* as = FindSection(sections, "async"))
	{
		FMemoryStream s((int)as->size, as->data);
		GC.getASyncRand().read(s);
	}
	st.turn = game->GetGameTurn();
	st.step = "done";
	m_inGame = true;
	Log(LOG_INFO, "load: game loaded at turn %d (%u bytes, reads + post-load %.0f ms, contexts + start %.0f ms, %d/%d context(s), %d Lua error(s), %d read mismatch(es))",
		st.turn, (unsigned)n, st.msRead, st.msStart, ss.LoadedContextCount(), (int)contexts.size(), st.luaErrors, st.mismatches);
	return true;
#undef LG_FAIL
}

bool RtSession::ResyncGame(const gc_host& host, const uint8_t* p, size_t n, std::vector<uint8_t>& abiState, LoadGameStatus& st)
{
	st = LoadGameStatus();
	st.step = "check";
	if (!m_inGame)
	{
		st.error = "no game is running: a resync replaces a running game (use gc_load_game first)";
		return false;
	}
	if (!CheckSave(p, n, st.error)) return false;
	// The game scripts keep state of their own (MapModData, caches): they restart with the save's game as after a load.
	m_engine.ClearNetQueues();
	if (m_scripts)
	{
		m_engine.SetScriptSystem(NULL);
		delete m_scripts;
		m_scripts = NULL;
	}
	m_inGame = false;
	m_newGameTried = false;
	return LoadGame(host, p, n, abiState, st);
}

// The runtime section of a save, for the replica digest (gc_state_digest -3).
void WriteRuntimeState(const RtSession& s, std::vector<uint8_t>& out)
{
	RtByteWriter w(out);
	RtSaveAccess::WriteRuntime(s, w);
}

}  // namespace lekmod_portable
