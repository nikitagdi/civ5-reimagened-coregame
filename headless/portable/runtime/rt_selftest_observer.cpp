// Scenario `observer` (task A9, milestone M3 and measurement M6; run.sh --scenario observer --turns N --seed S --ai A):
// RtSession::Load, NewGame({"seed":S,"observer":1,"ai_count":A}) (every civilization is an AI, an SS_OBSERVER slot is the active
// player), then N turns through RtSession::Pump. Checks: 0 Lua errors, no STUCK / GAME_OVER / WAITING_HUMAN, every turn ends.
// Measures per-turn wall time (median, max), the Load / Init / Init2 times and the total. Prints "observer-digest|turn|hex" lines
// (RtSession::StateDigest after every turn, outside the timing) so two processes can be compared; the final digest is in the report.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "CvDllContext.h"
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

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[768];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("observer: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

std::string Num(long long v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lld", v);
	return buf;
}

std::string Fix(double v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%.1f", v);
	return buf;
}

std::string Hex(uint64_t v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
	return buf;
}

double Median(std::vector<double> v)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double Max(const std::vector<double>& v)
{
	double m = 0;
	for (size_t i = 0; i < v.size(); ++i) m = std::max(m, v[i]);
	return m;
}

}  // namespace

extern "C" int rt_selftest_observer(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;

	// Left alive on purpose, as in scenario `load`.
	RtSession* session = new RtSession();
	RtSession::LoadStatus ls;
	double t0 = NowMs();
	const bool loaded = session->Load(*host, ls);
	const double msLoad = NowMs() - t0;
	c.add("load", loaded, "boot D1-D2 B1-B4 I1-I8 reached step %s (%.0f ms)%s%s", RtSession::StepName(ls.step), msLoad, ls.error.empty() ? "" : ": ", ls.error.c_str());

	RtSession::NewGameStatus st;
	std::vector<double> turnMs;
	std::vector<unsigned> turnSlices;
	std::vector<int> turnNet;
	std::string digests;  // JSON array body
	std::string finalDigest, mapDigest;
	std::string stopReason;
	double msTurns = 0, msFirst10 = 0, msLast10 = 0;
	int luaErrors = 0, luaErrorsBefore = 0, startTurn = -1, endTurn = -1, doneTurns = 0, majorsAlive = 0, citiesEnd = 0, unitsEnd = 0;
	int stuck = 0;
	uint64_t netTotal = 0;
	if (loaded)
	{
		char setup[128];
		snprintf(setup, sizeof(setup), "{\"game_type\":\"single\",\"seed\":%u,\"observer\":1,\"ai_count\":%u}", seed, ai);
		const bool started = session->NewGame(*host, setup, st);
		c.add("new_game", started, "%s reached step %s (Init %.0f ms, Init2 %.0f ms, L1-L6 %.0f ms, contexts %.0f ms, start %.0f ms)%s%s", setup,
			RtSession::NewGameStepName(st.step), st.msInit, st.msInit2, st.msLaunch, st.msContexts, st.msStart, st.error.empty() ? "" : ": ", st.error.c_str());
		c.add("lua_errors_start", started && st.luaErrorsMap == 0 && st.luaErrorsContexts == 0 && st.luaErrorsStart == 0, "%d / %d / %d Lua error(s) in map / contexts / start",
			st.luaErrorsMap, st.luaErrorsContexts, st.luaErrorsStart);
		mapDigest = Hex(st.mapDigest);
		RtScriptSystem* ss = session->Scripts();
		if (started && ss)
		{
			luaErrorsBefore = ss->GetErrorCount();
			startTurn = st.turn;
			int observers = 0, humans = 0;
			for (int i = 0; i < MAX_PLAYERS; ++i)
			{
				CvPlayer& p = GET_PLAYER((PlayerTypes)i);
				if (p.isObserver()) ++observers;  // an observer is not "alive"
				if (p.isAlive() && p.isHuman() && !p.isObserver()) ++humans;
			}
			c.add("observer_setup", observers == 1 && humans == 0 && session->Engine().GetActivePlayer() != NO_PLAYER, "%d observer slot(s), %d human(s), active player %d, %d major civ(s) alive",
				observers, humans, (int)session->Engine().GetActivePlayer(), st.majorsAlive);
			// Per-turn digests are taken outside the timed region.
			digests = "\"" + Hex(RtSession::StateDigest()) + "\"";
			printf("observer-digest|%d|%s\n", startTurn, Hex(RtSession::StateDigest()).c_str());
			const double tStart = NowMs();
			for (unsigned k = 0; k < turns; ++k)
			{
				RtSession::PumpResult r;
				double ms = 0;
				uint32_t slices = 0;
				int net = 0;
				for (;;)
				{
					r = session->Pump(RtSession::kMaxSlicesPerTurn, true);
					ms += r.ms;
					slices += r.slices;
					net += r.netMessages;
					if (r.status != RtSession::PUMP_AI_RUNNING || r.turnsAdvanced > 0) break;
				}
				if (r.status != RtSession::PUMP_AI_RUNNING || r.turnsAdvanced != 1)
				{
					stopReason = std::string(RtSession::PumpStatusName(r.status)) + " at turn " + Num(r.turn) + ": " + r.reason;
					if (r.status == RtSession::PUMP_STUCK) ++stuck;
					break;
				}
				turnMs.push_back(ms);
				turnSlices.push_back(slices);
				turnNet.push_back(net);
				netTotal += (uint64_t)net;
				++doneTurns;
				endTurn = r.turn;
				const std::string d = Hex(RtSession::StateDigest());
				digests += ",\"" + d + "\"";
				printf("observer-turn|%d|%u slices|%.1f ms|net %d|lua errors %d|digest %s\n", r.turn, (unsigned)slices, ms, net, ss->GetErrorCount() - luaErrorsBefore, d.c_str());
				finalDigest = d;
			}
			msTurns = NowMs() - tStart;
			luaErrors = ss->GetErrorCount() - luaErrorsBefore;
			for (size_t i = 0; i < turnMs.size() && i < 10; ++i) msFirst10 += turnMs[i];
			for (size_t i = turnMs.size() > 10 ? turnMs.size() - 10 : 0; i < turnMs.size(); ++i) msLast10 += turnMs[i];
			for (int i = 0; i < MAX_PLAYERS; ++i)
			{
				CvPlayer& p = GET_PLAYER((PlayerTypes)i);
				if (!p.isAlive()) continue;
				if (p.isMajorCiv() && !p.isObserver()) ++majorsAlive;
				citiesEnd += p.getNumCities();
				unitsEnd += p.getNumUnits();
			}
		}
	}

	const double med = Median(turnMs), mx = Max(turnMs);
	c.add("turns", loaded && doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) done (turn %d -> %d)%s%s", doneTurns, turns, startTurn, endTurn,
		stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0, "%d stuck turn(s)", stuck);
	c.add("lua_errors_turns", loaded && luaErrors == 0, "%d Lua error(s) during the turns", luaErrors);
	c.add("alive", majorsAlive >= 1 && citiesEnd > 0, "%d major civ(s) alive, %d city(ies), %d unit(s) at the end", majorsAlive, citiesEnd, unitsEnd);
	c.add("timing", doneTurns > 0, "turn wall time: median %.1f ms, max %.1f ms, mean %.1f ms, first 10 turns %.0f ms, last 10 turns %.0f ms; total %.0f ms for %d turn(s)", med, mx,
		doneTurns ? msTurns / doneTurns : 0.0, msFirst10, msLast10, msTurns, doneTurns);

	std::string perTurn, perSlices;
	for (size_t i = 0; i < turnMs.size(); ++i)
	{
		perTurn += (i ? "," : "") + Fix(turnMs[i]);
		perSlices += (i ? "," : "") + Num(turnSlices[i]);
	}
	std::string out = "{\"failed\":" + Num(c.failed) + ",\"seed\":" + Num(seed) + ",\"ai_count\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) +
		",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"map_digest\":\"" + mapDigest +
		"\",\"state_digest\":\"" + finalDigest + "\",\"lua_errors\":" + Num(luaErrors) + ",\"stuck\":" + Num(stuck) + ",\"stop_reason\":\"" + Escape(stopReason) +
		"\",\"net_messages\":" + Num((long long)netTotal) + ",\"ms\":{\"load\":" + Fix(msLoad) + ",\"init\":" + Fix(st.msInit) + ",\"init2\":" + Fix(st.msInit2) +
		",\"launch\":" + Fix(st.msLaunch) + ",\"contexts\":" + Fix(st.msContexts) + ",\"start\":" + Fix(st.msStart) + ",\"turns_total\":" + Fix(msTurns) +
		",\"turn_median\":" + Fix(med) + ",\"turn_max\":" + Fix(mx) + ",\"turn_mean\":" + Fix(doneTurns ? msTurns / doneTurns : 0.0) + ",\"first10\":" + Fix(msFirst10) +
		",\"last10\":" + Fix(msLast10) + "},\"turn_ms\":[" + perTurn + "],\"turn_slices\":[" + perSlices + "],\"digests\":[" + digests + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("observer-summary|seed %u|ai %u|turns %d|median %.1f ms|max %.1f ms|total %.0f ms|state %s|map %s\n", seed, ai, doneTurns, med, mx, msTurns, finalDigest.c_str(),
		mapDigest.c_str());
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
