// Scenario `dirtybench` (task N1.3 part Q_PLOTS_DIRTY, 10 (D5), 12 item 8; run.sh --scenario dirtybench --turns N --seed S --ai A):
// the cost of "dirty plots" on ABI v1 for a Huge map (WORLDSIZE_HUGE, 128x80 = 10240 plots) with 8 civilizations (--ai 7) and an observer.
// RtSession::Load, NewGame({"seed":S,"observer":1,"ai_count":A,"world_size":"WORLDSIZE_HUGE"}), then N AI turns through RtSession::Pump. After every turn,
// outside the turn timing, per living major civ's team:
//   (a) a full Q_PLOTS(team) through RunGameQuery (query + release only);
//   (b) a snapshot of every gcq_plot record plus the future-basis fields (plot type, terrain, feature, river edges and their flow, natural wonder)
//       and the comparison with the previous turn's snapshot. b_total = (a) + copy + basis + compare (everything a dirty-by-comparison scheme would pay);
//       b_extra = copy + basis + compare (what it pays on top of a Q_PLOTS it makes anyway);
//   (c) the number of changed plots (a plot counts once if any team's record or the basis changed; per-team counts are reported too);
//   (d) the AI turn time (RtSession::Pump wall time, the early number of AC-N5.4).
// The snapshot code lives here; the ABI is unchanged. Prints "dirtybench-turn|..." lines and a p50 / p95 / max table; the decision rule of the card
// (b_total > 3 ms on Huge x 8 teams -> conditional task D5) is printed as "dirtybench-decision|...".
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CvDllContext.h"
#include "CvGameCoreUtils.h"
#include "../abi/gc_query.h"
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
		printf("dirtybench: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
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
	snprintf(buf, sizeof(buf), "%.3f", v);
	return buf;
}

// Nearest-rank percentile of a copy of v.
double Pct(std::vector<double> v, double p)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	size_t idx = (size_t)(p * (double)(v.size() - 1) + 0.5);
	if (idx >= v.size()) idx = v.size() - 1;
	return v[idx];
}

double Max(const std::vector<double>& v)
{
	double m = 0;
	for (size_t i = 0; i < v.size(); ++i) m = std::max(m, v[i]);
	return m;
}

// The fields of a plot the future "basis" (static terrain) would carry.
struct Basis
{
	int16_t terrain;
	int16_t feature;
	int8_t plotType;
	int8_t flowE, flowSE, flowSW;
	uint8_t riverBits;
	uint8_t naturalWonder;
};

// Series statistics for the table.
struct Series
{
	std::string name;
	std::vector<double> v;
	Series(const char* n = "") : name(n) {}
};

void PrintRow(const Series& s, const char* unit)
{
	printf("dirtybench-table|%-22s|p50 %9.3f %s|p95 %9.3f %s|max %9.3f %s\n", s.name.c_str(), Pct(s.v, 0.5), unit, Pct(s.v, 0.95), unit, Max(s.v), unit);
}

std::string RowJson(const Series& s)
{
	return "\"" + s.name + "\":{\"p50\":" + Fix(Pct(s.v, 0.5)) + ",\"p95\":" + Fix(Pct(s.v, 0.95)) + ",\"max\":" + Fix(Max(s.v)) + "}";
}

}  // namespace

extern "C" int rt_selftest_dirtybench(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;

	RtSession* session = new RtSession();  // left alive on purpose, as in scenario `observer`
	RtSession::LoadStatus ls;
	const bool loaded = session->Load(*host, ls);
	c.add("load", loaded, "boot reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());

	RtSession::NewGameStatus st;
	Series sA("a_q_plots"), sBtotal("b_total"), sBextra("b_extra"), sBcopy("b_copy"), sBbasis("b_basis"), sBcmp("b_compare"), sCunion("c_changed_union"),
		sCteam("c_changed_per_team_mean"), sCteamMax("c_changed_per_team_max"), sCbasis("c_changed_basis"), sD("d_ai_turn_ms");
	std::string stopReason;
	int luaErrors = 0, luaErrorsBefore = 0, startTurn = -1, endTurn = -1, doneTurns = 0, stuck = 0, plotCount = 0, teamCount = 0;
	if (loaded)
	{
		char setup[160];
		snprintf(setup, sizeof(setup), "{\"game_type\":\"single\",\"seed\":%u,\"observer\":1,\"ai_count\":%u,\"world_size\":\"WORLDSIZE_HUGE\"}", seed, ai);
		const bool started = session->NewGame(*host, setup, st);
		c.add("new_game", started, "%s reached step %s%s%s", setup, RtSession::NewGameStepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());
		RtScriptSystem* ss = session->Scripts();
		if (started && ss)
		{
			luaErrorsBefore = ss->GetErrorCount();
			startTurn = st.turn;
			CvMap& map = GC.getMap();
			const int width = (int)map.getGridWidth(), height = (int)map.getGridHeight();
			plotCount = width * height;
			// The viewers: the team of every living major civ (not the observer).
			std::vector<int32_t> teams;
			for (int i = 0; i < MAX_PLAYERS; ++i)
			{
				CvPlayer& p = GET_PLAYER((PlayerTypes)i);
				if (p.isObserver() || !p.isAlive() || !p.isMajorCiv()) continue;
				const int32_t t = (int32_t)p.getTeam();
				if (std::find(teams.begin(), teams.end(), t) == teams.end()) teams.push_back(t);
			}
			teamCount = (int)teams.size();
			// The Lekmap Pangaea script sets its own Huge size (82x74 = 6068 plots, GetMapInitData); the card's 10240 is the vanilla 128x80.
			// The cost is linear in the plot count, so the decision line also prints the figure scaled to 10240.
			c.add("huge_x8", (plotCount == 6068 || plotCount == 10240) && teamCount == 8, "%dx%d = %d plots, %d viewer team(s) (expect 6068 [Lekmap Huge] or 10240, and 8)", width, height, plotCount, teamCount);

			std::vector<std::vector<gcq_plot> > prev(teams.size()), cur(teams.size());
			std::vector<Basis> prevBasis, curBasis;
			std::vector<unsigned char> dirty((size_t)plotCount);
			bool havePrev = false;
			RtSession::BlockTarget none;

			// One measured round at the current game state. Returns false when a query failed.
			struct Round
			{
				static bool Run(const std::vector<int32_t>& teams, int width, int height, std::vector<std::vector<gcq_plot> >& cur, std::vector<std::vector<gcq_plot> >& prev,
					std::vector<Basis>& curBasis, std::vector<Basis>& prevBasis, std::vector<unsigned char>& dirty, bool havePrev, const RtSession::BlockTarget& none,
					double& msA, double& msCopy, double& msBasis, double& msCmp, int& changedUnion, double& changedTeamMean, int& changedTeamMax, int& changedBasis)
				{
					const size_t n = (size_t)width * (size_t)height;
					msA = msCopy = msBasis = msCmp = 0;
					// (a) + the copy into the snapshot.
					for (size_t k = 0; k < teams.size(); ++k)
					{
						gc_buf b = {0, 0, 0, 0};
						std::string msg;
						const int32_t viewer = teams[k];
						double t0 = NowMs();
						if (RunGameQuery(GC_Q_PLOTS, &viewer, 1, -1, -1, none, &b, msg) != GC_OK) return false;
						msA += NowMs() - t0;
						t0 = NowMs();
						gcq_header h;
						memcpy(&h, b.data, sizeof(h));
						if (h.count != n || h.record_size != sizeof(gcq_plot)) { gc_buf_release(&b); return false; }
						cur[k].resize(n);
						memcpy(&cur[k][0], b.data + sizeof(h), n * sizeof(gcq_plot));
						msCopy += NowMs() - t0;
						gc_buf_release(&b);
					}
					// Basis fields, once for the whole map (they do not depend on the viewer).
					double t0 = NowMs();
					curBasis.resize(n);
					CvMap& map = GC.getMap();
					for (int y = 0; y < height; ++y)
					{
						for (int x = 0; x < width; ++x)
						{
							const CvPlot* plot = map.plot(x, y);
							Basis& bs = curBasis[(size_t)y * (size_t)width + (size_t)x];
							bs.terrain = (int16_t)plot->getTerrainType();
							bs.feature = (int16_t)plot->getFeatureType();
							bs.plotType = (int8_t)plot->getPlotType();
							bs.flowE = (int8_t)plot->getRiverEFlowDirection();
							bs.flowSE = (int8_t)plot->getRiverSEFlowDirection();
							bs.flowSW = (int8_t)plot->getRiverSWFlowDirection();
							bs.riverBits = (uint8_t)((plot->isWOfRiver() ? 1 : 0) | (plot->isNWOfRiver() ? 2 : 0) | (plot->isNEOfRiver() ? 4 : 0));
							bs.naturalWonder = plot->IsNaturalWonder() ? 1 : 0;
						}
					}
					msBasis = NowMs() - t0;
					// Comparison with the previous snapshot.
					t0 = NowMs();
					changedUnion = 0;
					changedBasis = 0;
					changedTeamMax = 0;
					long long teamSum = 0;
					if (havePrev)
					{
						memset(&dirty[0], 0, n);
						for (size_t k = 0; k < teams.size(); ++k)
						{
							int cnt = 0;
							const gcq_plot* a = &cur[k][0];
							const gcq_plot* p = &prev[k][0];
							for (size_t i = 0; i < n; ++i)
							{
								if (memcmp(&a[i], &p[i], sizeof(gcq_plot)) != 0) { ++cnt; dirty[i] = 1; }
							}
							teamSum += cnt;
							changedTeamMax = std::max(changedTeamMax, cnt);
						}
						for (size_t i = 0; i < n; ++i)
						{
							if (memcmp(&curBasis[i], &prevBasis[i], sizeof(Basis)) != 0) { ++changedBasis; dirty[i] = 1; }
							if (dirty[i]) ++changedUnion;
						}
					}
					msCmp = NowMs() - t0;
					changedTeamMean = teams.empty() ? 0.0 : (double)teamSum / (double)teams.size();
					prev.swap(cur);
					prevBasis.swap(curBasis);
					return true;
				}
			};

			// Baseline snapshot of the starting state (not measured).
			{
				double a, b, d, e, m;
				int u, tm, cb;
				if (!Round::Run(teams, width, height, cur, prev, curBasis, prevBasis, dirty, false, none, a, b, d, e, u, m, tm, cb))
					stopReason = "baseline Q_PLOTS failed";
				havePrev = stopReason.empty();
			}

			for (unsigned k = 0; k < turns && stopReason.empty(); ++k)
			{
				RtSession::PumpResult r;
				double ms = 0;
				for (;;)
				{
					r = session->Pump(RtSession::kMaxSlicesPerTurn, true);
					ms += r.ms;
					if (r.status != RtSession::PUMP_AI_RUNNING || r.turnsAdvanced > 0) break;
				}
				if (r.status != RtSession::PUMP_AI_RUNNING || r.turnsAdvanced != 1)
				{
					stopReason = std::string(RtSession::PumpStatusName(r.status)) + " at turn " + Num(r.turn) + ": " + r.reason;
					if (r.status == RtSession::PUMP_STUCK) ++stuck;
					break;
				}
				double a, cp, bs, cm, mean;
				int uni, tmax, cbasis;
				if (!Round::Run(teams, width, height, cur, prev, curBasis, prevBasis, dirty, havePrev, none, a, cp, bs, cm, uni, mean, tmax, cbasis))
				{
					stopReason = "Q_PLOTS failed at turn " + Num(r.turn);
					break;
				}
				sA.v.push_back(a);
				sBcopy.v.push_back(cp);
				sBbasis.v.push_back(bs);
				sBcmp.v.push_back(cm);
				sBextra.v.push_back(cp + bs + cm);
				sBtotal.v.push_back(a + cp + bs + cm);
				sCunion.v.push_back(uni);
				sCteam.v.push_back(mean);
				sCteamMax.v.push_back(tmax);
				sCbasis.v.push_back(cbasis);
				sD.v.push_back(ms);
				++doneTurns;
				endTurn = r.turn;
				printf("dirtybench-turn|%d|a %.3f ms|b_total %.3f ms|b_extra %.3f ms (copy %.3f, basis %.3f, cmp %.3f)|changed union %d, per-team mean %.1f max %d, basis %d|ai turn %.1f ms\n",
					r.turn, a, a + cp + bs + cm, cp + bs + cm, cp, bs, cm, uni, mean, tmax, cbasis, ms);
			}
			luaErrors = ss->GetErrorCount() - luaErrorsBefore;
		}
	}

	c.add("turns", loaded && doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) done (turn %d -> %d)%s%s", doneTurns, turns, startTurn, endTurn,
		stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0, "%d stuck turn(s)", stuck);
	c.add("lua_errors_turns", loaded && luaErrors == 0, "%d Lua error(s) during the turns", luaErrors);

	PrintRow(sA, "ms");
	PrintRow(sBtotal, "ms");
	PrintRow(sBextra, "ms");
	PrintRow(sBcopy, "ms");
	PrintRow(sBbasis, "ms");
	PrintRow(sBcmp, "ms");
	PrintRow(sCunion, "plots");
	PrintRow(sCteam, "plots");
	PrintRow(sCteamMax, "plots");
	PrintRow(sCbasis, "plots");
	PrintRow(sD, "ms");
	const double bP50 = Pct(sBtotal.v, 0.5), bP95 = Pct(sBtotal.v, 0.95), bMax = Max(sBtotal.v);
	const double scale = plotCount > 0 ? 10240.0 / (double)plotCount : 1.0;
	const bool needD5 = bP50 > 3.0 || bP50 * scale > 3.0;
	printf("dirtybench-decision|b_total p50 %.3f ms, p95 %.3f ms, max %.3f ms (%d plots x %d teams); scaled to 10240 plots: p50 %.3f, p95 %.3f, max %.3f ms|rule: b > 3 ms -> conditional task D5|%s\n",
		bP50, bP95, bMax, plotCount, teamCount, bP50 * scale, bP95 * scale, bMax * scale, needD5 ? "D5 NEEDED (ask the user)" : "D5 not needed");

	std::string out = "{\"failed\":" + Num(c.failed) + ",\"seed\":" + Num(seed) + ",\"ai_count\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) + ",\"turns_done\":" + Num(doneTurns) +
		",\"plots\":" + Num(plotCount) + ",\"teams\":" + Num(teamCount) + ",\"lua_errors\":" + Num(luaErrors) + ",\"stuck\":" + Num(stuck) + ",\"stop_reason\":\"" + Escape(stopReason) +
		"\",\"decision_d5\":" + (needD5 ? "true" : "false") + ",\"stats\":{" + RowJson(sA) + "," + RowJson(sBtotal) + "," + RowJson(sBextra) + "," + RowJson(sBcopy) + "," +
		RowJson(sBbasis) + "," + RowJson(sBcmp) + "," + RowJson(sCunion) + "," + RowJson(sCteam) + "," + RowJson(sCteamMax) + "," + RowJson(sCbasis) + "," + RowJson(sD) + "},\"checks\":[" +
		c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
