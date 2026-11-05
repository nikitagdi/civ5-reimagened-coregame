// Scenario `dirty` (task N3.8; run.sh --scenario dirty --humans N --ai A --turns T [--world-size WORLDSIZE_*]): Q_PLOTS_DIRTY (21), Q_PLOTS with plot indices
// and Q_PLOT_BASIS (19) on a network game with N test-bot humans (the `mp` game, through the C ABI only).
// After every gc_pump call of the game the scenario asks Q_PLOTS_DIRTY, then reads the full Q_PLOTS of every watched team (the team of each human) and the full Q_PLOT_BASIS, and
// compares them with the previous step's. Checks:
//  - misses = 0: a plot whose Q_PLOTS record (any team) or basis record changed is in the dirty set of that step (this is the criterion that matters);
//  - the first call lists every plot; an immediate second call lists none (the call resets the set: "non-empty only if something changed");
//  - Q_PLOTS(team, indices) and Q_PLOT_BASIS(indices) of the dirty plots equal those rows of the full replies; a bad index is INVALID_ARG; Q_PLOT_BASIS has no fog (an
//    unrevealed plot has its terrain) and its fields agree with Q_PLOTS where both speak (type, terrain, river edges, natural wonder, lake, coast);
//  - at the end: a change of one plot's improvement dirties it and not all; a change of a plot type dirties every plot (LAKE / COASTAL are derived from it); a team that
//    enters the watched set (a slot taken) gets every plot; the first call after a Reset lists every plot (a gc_load_game starts with no snapshot, the same state).
// The report holds the cost of the call (p50 / p95 / max ms) and the snapshot size; on Huge with 8 humans it is the number of D5 when above 3 ms).
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "../abi/gc_query_dirty.h"
#include "rt_active.h"
#include "rt_selftest.h"
#include "rt_session.h"

using namespace lekmod_portable;

namespace {

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
		printf("dirty: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings, luaErrors;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0) {}
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
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 20) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
}

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

double Percentile(std::vector<double> v, double q)
{
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	size_t i = (size_t)(q * (double)(v.size() - 1) + 0.5);
	if (i >= v.size()) i = v.size() - 1;
	return v[i];
}

// A reply held with its header.
struct Reply
{
	gc_buf buf;
	gcq_header h;
	int32_t rc;
	std::string err;
	Reply() : rc(-99) { memset(&buf, 0, sizeof(buf)); memset(&h, 0, sizeof(h)); }
	~Reply() { gc_buf_release(&buf); }
	bool Ask(gc_instance* inst, int kind, const int32_t* args, unsigned argc)
	{
		gc_buf_release(&buf);
		memset(&h, 0, sizeof(h));
		gc_error e;
		memset(&e, 0, sizeof(e));
		rc = gc_query(inst, kind, args, argc, &buf, &e);
		err = e.message;
		if (rc != GC_OK || buf.len < sizeof(h)) return false;
		memcpy(&h, buf.data, sizeof(h));
		return h.kind == kind;
	}
	const uint8_t* Rec(uint32_t i) const { return buf.data + sizeof(h) + (size_t)i * h.record_size; }
};

// The full Q_PLOTS of a team / the full Q_PLOT_BASIS as raw records, one vector entry (record_size bytes) per plot.
bool ReadAll(gc_instance* inst, int kind, int team, std::vector<uint8_t>& out, uint32_t& recSize, uint32_t expect)
{
	Reply r;
	const int32_t a[1] = {(int32_t)team};
	if (!r.Ask(inst, kind, kind == GC_Q_PLOTS ? a : NULL, kind == GC_Q_PLOTS ? 1 : 0) || r.h.count != expect) return false;
	recSize = r.h.record_size;
	out.assign(r.Rec(0), r.Rec(0) + (size_t)r.h.count * r.h.record_size);
	return true;
}

gc_command Cmd(int kind, int player, const int32_t* args, unsigned argc)
{
	gc_command c;
	memset(&c, 0, sizeof(c));
	c.kind = (uint16_t)kind;
	c.player = (uint8_t)player;
	c.argc = argc;
	c.args = args;
	return c;
}

}  // namespace

extern "C" int rt_selftest_dirty(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* world_size, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const double t0 = NowMs();
	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	int doneTurns = 0, startTurn = -1, endTurn = -1, steps = 0, stuck = 0, pumpErrors = 0, misses = 0, missPlots = 0, keyframeOk = 0, keyframeBad = 0, idleChecks = 0, idleNonEmpty = 0;
	int indexChecks = 0, indexBad = 0, basisBad = 0, dirtySum = 0, changedSum = 0, falseDirty = 0, dirtyAllSteps = 0, calls = 0;
	size_t plotCount = 0;
	int teamCount = 0;
	std::string stopReason, firstMiss, indexFirstBad, basisFirstBad;
	std::vector<double> dirtyMs, fullMs;
	std::vector<int> humanList, teams;
	size_t snapshotBytes = 0;
	std::string tail;
	if (inst)
	{
		std::string players;
		for (unsigned i = 0; i < humans; ++i)
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		for (unsigned i = 0; i < ai; ++i) players += std::string(humans || i ? "," : "") + "{\"kind\":\"ai\",\"civ\":\"random\"}";
		std::string slots;
		for (unsigned i = 0; i < humans; ++i) slots += Format("%s%u", i ? "," : "", i);
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"autoplay_humans\":[%s],\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed,
			players.c_str(), slots.c_str(), world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		const double t1 = NowMs();
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s (%.0f ms)", setup.c_str(), rc, err.message, NowMs() - t1);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			CvGame& game = GC.getGame();
			CvMap& map = GC.getMap();
			plotCount = (size_t)map.numPlots();
			for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
				if (IsSeatedHuman(i))
				{
					humanList.push_back(i);
					const int t = (int)GET_PLAYER((PlayerTypes)i).getTeam();
					if (std::find(teams.begin(), teams.end(), t) == teams.end()) teams.push_back(t);
				}
			teamCount = (int)teams.size();
			std::sort(teams.begin(), teams.end());
			c.add("setup", teamCount == (int)humans && plotCount > 0, "%dx%d = %u plots, %d human(s) in %d watched team(s)", (int)map.getGridWidth(), (int)map.getGridHeight(), (unsigned)plotCount, (int)humanList.size(), teamCount);

			std::vector<std::vector<uint8_t> > prev(teams.size());
			std::vector<uint8_t> prevBasis;
			uint32_t plotRec = 0, basisRec = 0;
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = game.getGameTurn();
			int turn = startTurn;
			const int target = startTurn + (int)turns;
			const long long maxCalls = 20000LL * (turns + 1);
			bool first = true;
			while (turn < target)
			{
				if (++calls > maxCalls) { stopReason = Format("turn %d: %lld gc_pump calls without reaching turn %d", turn, maxCalls, target); break; }
				const int32_t prc = gc_pump(inst, 64, &pr, &err);
				if (pr.status == GC_PUMP_STUCK) { ++stuck; stopReason = Format("STUCK at turn %d: %s", pr.turn, err.message); break; }
				if (pr.status == GC_PUMP_GAME_OVER) { stopReason = Format("GAME_OVER at turn %d", pr.turn); break; }
				if (prc != GC_OK) { ++pumpErrors; stopReason = Format("gc_pump at turn %d: %d %s", pr.turn, prc, err.message); break; }
				++steps;

				// 1. the dirty set of this step
				Reply d;
				double ts = NowMs();
				const bool dOk = d.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0);
				dirtyMs.push_back(NowMs() - ts);
				if (!dOk || d.h.record_size < sizeof(gcq_plot_dirty)) { stopReason = Format("Q_PLOTS_DIRTY: rc %d %s", d.rc, d.err.c_str()); break; }
				std::vector<unsigned char> inSet(plotCount, 0);
				for (uint32_t i = 0; i < d.h.count; ++i)
				{
					gcq_plot_dirty rec;
					memcpy(&rec, d.Rec(i), sizeof(rec));
					if (rec.index < plotCount) inSet[rec.index] = 1;
				}
				dirtySum += (int)d.h.count;
				if (d.h.count == plotCount) ++dirtyAllSteps;
				if (first)
				{
					if (d.h.count == plotCount) ++keyframeOk; else ++keyframeBad;
				}
				// 2. the full replies and the comparison with the previous step
				ts = NowMs();
				std::vector<std::vector<uint8_t> > cur(teams.size());
				std::vector<uint8_t> curBasis;
				bool ok = true;
				for (size_t t = 0; t < teams.size() && ok; ++t) ok = ReadAll(inst, GC_Q_PLOTS, teams[t], cur[t], plotRec, (uint32_t)plotCount);
				ok = ok && ReadAll(inst, GC_Q_PLOT_BASIS, 0, curBasis, basisRec, (uint32_t)plotCount);
				fullMs.push_back(NowMs() - ts);
				if (!ok || plotRec < sizeof(gcq_plot) || basisRec < sizeof(gcq_plot_basis)) { stopReason = "a full Q_PLOTS or Q_PLOT_BASIS failed"; break; }
				if (!first)
				{
					int changedThisStep = 0, missThisStep = 0, falseThisStep = 0;
					for (size_t i = 0; i < plotCount; ++i)
					{
						bool changed = memcmp(&curBasis[i * basisRec], &prevBasis[i * basisRec], basisRec) != 0;
						for (size_t t = 0; t < teams.size() && !changed; ++t) changed = memcmp(&cur[t][i * plotRec], &prev[t][i * plotRec], plotRec) != 0;
						if (changed) ++changedThisStep;
						if (changed && !inSet[i])
						{
							++missThisStep;
							if (firstMiss.empty())
							{
								gcq_plot pl;
								memcpy(&pl, &cur[0][i * plotRec], sizeof(pl));
								firstMiss = Format("turn %d step %d plot %u (%d,%d)", pr.turn, steps, (unsigned)i, (int)pl.x, (int)pl.y);
							}
						}
						if (!changed && inSet[i]) ++falseThisStep;
					}
					misses += missThisStep;
					if (missThisStep > 0) ++missPlots;
					changedSum += changedThisStep;
					if (d.h.count != plotCount) falseDirty += falseThisStep;
				}
				// 3. the index forms equal the rows of the full replies
				if (d.h.count > 0 && d.h.count < plotCount)
				{
					std::vector<int32_t> idx;
					for (size_t i = 0; i < plotCount && idx.size() < 48; ++i) if (inSet[i]) idx.push_back((int32_t)i);
					for (size_t t = 0; t < teams.size() && t < 2; ++t)
					{
						std::vector<int32_t> a;
						a.push_back(teams[t]);
						a.insert(a.end(), idx.begin(), idx.end());
						Reply pi;
						++indexChecks;
						bool bad = !pi.Ask(inst, GC_Q_PLOTS, &a[0], (unsigned)a.size()) || pi.h.count != idx.size() || pi.h.record_size != plotRec;
						for (size_t k = 0; !bad && k < idx.size(); ++k) bad = memcmp(pi.Rec((uint32_t)k), &cur[t][(size_t)idx[k] * plotRec], plotRec) != 0;
						if (bad) { ++indexBad; if (indexFirstBad.empty()) indexFirstBad = Format("Q_PLOTS(team %d, %u indices) at turn %d", teams[t], (unsigned)idx.size(), pr.turn); }
					}
					Reply bi;
					bool bad = !bi.Ask(inst, GC_Q_PLOT_BASIS, &idx[0], (unsigned)idx.size()) || bi.h.count != idx.size() || bi.h.record_size != basisRec;
					for (size_t k = 0; !bad && k < idx.size(); ++k) bad = memcmp(bi.Rec((uint32_t)k), &curBasis[(size_t)idx[k] * basisRec], basisRec) != 0;
					++indexChecks;
					if (bad) { ++indexBad; if (indexFirstBad.empty()) indexFirstBad = Format("Q_PLOT_BASIS(%u indices) at turn %d", (unsigned)idx.size(), pr.turn); }
				}
				// 4. the call resets the set: asking again at once gives nothing
				if (steps % 3 == 1)
				{
					Reply again;
					++idleChecks;
					if (!again.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0) || again.h.count != 0) ++idleNonEmpty;
				}
				// 5. the basis against Q_PLOTS of the whole truth (viewer -1) where both speak
				if (first || steps % 10 == 0)
				{
					std::vector<uint8_t> truth;
					uint32_t truthRec = 0;
					if (ReadAll(inst, GC_Q_PLOTS, -1, truth, truthRec, (uint32_t)plotCount) && truthRec >= sizeof(gcq_plot))
					{
						int bad = 0;
						for (size_t i = 0; i < plotCount; ++i)
						{
							gcq_plot a;
							gcq_plot_basis b;
							memcpy(&a, &truth[i * truthRec], sizeof(a));
							memcpy(&b, &curBasis[i * basisRec], sizeof(b));
							const unsigned rivers = (a.flags & GC_PLOT_F_RIVER_MASK) >> GC_PLOT_F_RIVER_SHIFT;
							const bool wonder = (a.flags & GC_PLOT_F_NATURAL_WONDER) != 0;
							const int x = (int)(i % (size_t)map.getGridWidth()), y = (int)(i / (size_t)map.getGridWidth());
							if (b.index != (uint32_t)i || a.x != x || a.y != y || b.plot_type != a.plot_type || b.terrain != a.terrain || b.continent_art != (uint8_t)a.continent_art || b.rivers != rivers ||
								((b.flags & GC_BASIS_F_LAKE) != 0) != ((a.flags & GC_PLOT_F_LAKE) != 0) || ((b.flags & GC_BASIS_F_COASTAL) != 0) != ((a.flags & GC_PLOT_F_COASTAL) != 0) ||
								((b.flags & GC_BASIS_F_NATURAL_WONDER) != 0) != wonder || (wonder && b.feature != a.feature) || (b.feature >= 0 && b.feature != a.feature))
							{
								if (++bad == 1 && basisFirstBad.empty()) basisFirstBad = Format("plot %u (%d,%d): basis type %d terrain %d rivers %02x flags %x vs Q_PLOTS(-1) type %d terrain %d rivers %02x flags %x", (unsigned)i, x, y,
									(int)b.plot_type, (int)b.terrain, (unsigned)b.rivers, (unsigned)b.flags, (int)a.plot_type, (int)a.terrain, rivers, (unsigned)a.flags);
							}
						}
						basisBad += bad;
					}
					else ++basisBad;
				}
				snapshotBytes = 0;
				prev = cur;
				prevBasis = curBasis;
				first = false;
				if (pr.turn != turn)
				{
					doneTurns += pr.turn - turn;
					turn = pr.turn;
				}
			}
			endTurn = turn;

			// ---- scenario-only probes on the final state ----
			if (stopReason.empty())
			{
				PlotDirtyTracker t1;
				std::string tmsg;
				{
					// Reset and the first call: a stand-alone tracker (an instance that was just created or loaded has none).
					gc_buf b1 = {0, 0, 0, 0}, b2 = {0, 0, 0, 0}, b3 = {0, 0, 0, 0};
					const bool a1 = t1.Query(&b1, tmsg) == GC_OK;
					const int n1 = t1.LastCount();
					const bool a2 = t1.Query(&b2, tmsg) == GC_OK;
					const int n2 = t1.LastCount();
					t1.Reset();
					const bool a3 = t1.Query(&b3, tmsg) == GC_OK;
					const int n3 = t1.LastCount();
					gc_buf_release(&b1);
					gc_buf_release(&b2);
					gc_buf_release(&b3);
					c.add("keyframes", a1 && a2 && a3 && n1 == (int)plotCount && n2 == 0 && n3 == (int)plotCount, "a fresh tracker lists %d of %u plots, again %d, after Reset %d (snapshot %u bytes)", n1, (unsigned)plotCount, n2, n3,
						(unsigned)t1.SnapshotBytes());
					snapshotBytes = t1.SnapshotBytes();
				}
				Reply base;
				base.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0);  // the instance's snapshot is at the final state
				// A change of one plot's improvement: that plot (and not every plot) is dirty.
				{
					const int t = teams.empty() ? 0 : teams[0];
					CvPlot* target = NULL;
					for (size_t i = 0; i < plotCount && !target; ++i)
					{
						CvPlot* p = map.plotByIndex((int)i);
						if (p && !p->isWater() && !p->isCity() && !p->isMountain() && p->getImprovementType() == NO_IMPROVEMENT && p->isVisible((TeamTypes)t, false)) target = p;
					}
					const ImprovementTypes farm = (ImprovementTypes)GC.getInfoTypeForString("IMPROVEMENT_FARM", true);
					int count = -1;
					bool listed = false;
					if (target && farm != NO_IMPROVEMENT)
					{
						target->setImprovementType(farm, NO_PLAYER);
						Reply r;
						if (r.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0))
						{
							count = (int)r.h.count;
							for (uint32_t i = 0; i < r.h.count; ++i)
							{
								gcq_plot_dirty rec;
								memcpy(&rec, r.Rec(i), sizeof(rec));
								if ((int)rec.index == target->getY() * map.getGridWidth() + target->getX()) listed = true;
							}
						}
						target->setImprovementType(NO_IMPROVEMENT, NO_PLAYER);
						Reply back;
						back.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0);  // the undo is a change too: swallow it
					}
					c.add("one_plot_change", listed && count > 0 && count < (int)plotCount / 2, "an improvement set on a visible plot: %d plot(s) dirty (of %u), the plot is %s", count, (unsigned)plotCount, listed ? "listed" : "NOT listed");
				}
				// A team enters the watched set (a slot taken): every plot. Emulated with the pregame slot of an AI major; undone after.
				{
					int ai2 = -1;
					for (int i = 0; i < MAX_MAJOR_CIVS && ai2 < 0; ++i)
					{
						const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
						if (p.isAlive() && !p.isHuman() && !p.isObserver()) ai2 = i;
					}
					int count = -1, after = -1;
					if (ai2 >= 0)
					{
						const SlotStatus was = CvPreGame::slotStatus((PlayerTypes)ai2);
						CvPreGame::setSlotStatus((PlayerTypes)ai2, SS_TAKEN);
						Reply r;
						if (r.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0)) count = (int)r.h.count;
						CvPreGame::setSlotStatus((PlayerTypes)ai2, was);
						Reply r2;
						if (r2.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0)) after = (int)r2.h.count;  // the team leaves again: a keyframe too (the set changed)
					}
					c.add("team_enters", ai2 >= 0 && count == (int)plotCount, "a slot taken by a human (player %d): %d of %u plots dirty (the view of the new team needs a keyframe); the call after it left again: %d", ai2, count, (unsigned)plotCount, after);
				}
				// A plot type changes: every plot is dirty (LAKE / COASTAL are derived from the type and the landmass).
				{
					CvPlot* target = NULL;
					for (size_t i = 0; i < plotCount && !target; ++i)
					{
						CvPlot* p = map.plotByIndex((int)i);
						if (p && p->getPlotType() == PLOT_OCEAN && !p->isLake() && p->getNumUnits() == 0) target = p;
					}
					int count = -1;
					if (target)
					{
						Reply settle;
						settle.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0);
						target->setPlotType(PLOT_LAND, true, false);
						Reply r;
						if (r.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0)) count = (int)r.h.count;
						target->setPlotType(PLOT_OCEAN, true, false);
					}
					c.add("plot_type_change", target != NULL && count == (int)plotCount, "an ocean plot made land (scenario-only map edit): %d of %u plots dirty", count, (unsigned)plotCount);
				}
				// P13 (task N3.19): the fields appended to the records of Q_PLOTS / Q_UNITS / Q_CITIES / Q_PLAYERS, Q_CITY_ART, Q_TRADE_ROUTES and the art technologies of
				// Q_CATALOG; the plot fields also go through the dirty set (a scenario-only edit of the build progress and of a pillaged route).
				{
					std::string bad;
					int sizeBad = 0, unitBad = 0, cityBad = 0, artBad = 0, playerBad = 0, workedBad = 0, editBad = 0;
					int units = 0, cities = 0, artRecords = 0, routes = 0, artTechCount = 0, worked = 0;
					const int t = teams.empty() ? 0 : teams[0];
					{
						Reply r;
						if (!r.Ask(inst, GC_Q_PLOTS, &t, 1) || r.h.record_size < 32) { ++sizeBad; bad += " Q_PLOTS record;"; }
						if (!r.Ask(inst, GC_Q_UNITS, &t, 1) || r.h.record_size < 44) { ++sizeBad; bad += " Q_UNITS record;"; }
						if (!r.Ask(inst, GC_Q_CITIES, &t, 1) || r.h.record_size < 56) { ++sizeBad; bad += " Q_CITIES record;"; }
						if (!r.Ask(inst, GC_Q_PLAYERS, &t, 1) || r.h.record_size < 48) { ++sizeBad; bad += " Q_PLAYERS record;"; }
						if (!r.Ask(inst, GC_Q_CITY_ART, &t, 1) || r.h.record_size != 12) { ++sizeBad; bad += Format(" Q_CITY_ART rc %d record %u;", r.rc, (unsigned)r.h.record_size); }
						if (!r.Ask(inst, GC_Q_TRADE_ROUTES, &t, 1) || r.h.record_size != 16) { ++sizeBad; bad += Format(" Q_TRADE_ROUTES rc %d record %u;", r.rc, (unsigned)r.h.record_size); }
					}
					{
						// Q_UNITS: facing is a direction, a unit in cargo names its transport, the build is a build.
						Reply r;
						const int32_t all[1] = {-1};
						if (r.Ask(inst, GC_Q_UNITS, all, 1) && r.h.record_size >= 44)
							for (uint32_t i = 0; i < r.h.count; ++i)
							{
								gcq_unit u;
								memcpy(&u, r.Rec(i), sizeof(u));
								++units;
								if (u.facing < 0 || u.facing >= NUM_DIRECTION_TYPES || ((u.flags & GC_UNIT_F_CARGO) != 0) != (u.transport_id >= 0) || u.build_type < -1 || u.build_type >= GC.getNumBuildInfos()) ++unitBad;
							}
						else ++unitBad;
						// Q_CITIES: the founder of a city seen now is a player.
						if (r.Ask(inst, GC_Q_CITIES, all, 1) && r.h.record_size >= 56)
							for (uint32_t i = 0; i < r.h.count; ++i)
							{
								gcq_city c2;
								memcpy(&c2, r.Rec(i), sizeof(c2));
								++cities;
								if (((c2.flags & GC_CITY_F_VISIBLE) != 0) != (c2.founder >= 0) || c2.founder >= MAX_PLAYERS) ++cityBad;
							}
						else ++cityBad;
						// Q_CITY_ART: every pair is a city with that building and the building has art; the count equals the one counted here.
						int expect = 0;
						for (int i = 0; i < MAX_PLAYERS; ++i)
						{
							CvPlayer& pl = GET_PLAYER((PlayerTypes)i);
							if (!pl.isEverAlive()) continue;
							int loop;
							for (CvCity* city = pl.firstCity(&loop); city; city = pl.nextCity(&loop))
								for (int b = 0; b < GC.getNumBuildingInfos(); ++b)
								{
									const CvBuildingEntry* info = GC.getBuildingInfo((BuildingTypes)b);
									const char* tag = info ? info->GetArtDefineTag() : NULL;
									if (tag && tag[0] && city->GetCityBuildings()->GetNumBuilding((BuildingTypes)b) > 0) ++expect;
								}
						}
						if (r.Ask(inst, GC_Q_CITY_ART, all, 1) && r.h.record_size == 12)
						{
							artRecords = (int)r.h.count;
							if ((int)r.h.count != expect) ++artBad;
							for (uint32_t i = 0; i < r.h.count; ++i)
							{
								gcq_city_art a;
								memcpy(&a, r.Rec(i), sizeof(a));
								const CvPlayer& owner = GET_PLAYER((PlayerTypes)a.owner);
								const CvCity* city = a.owner >= 0 ? owner.getCity(a.city_id) : NULL;
								if (!city || city->getX() != a.x || city->getY() != a.y || city->GetCityBuildings()->GetNumBuilding((BuildingTypes)a.building) <= 0) ++artBad;
							}
						}
						else ++artBad;
						// Q_TRADE_ROUTES: every route of the core for the whole truth.
						CvGameTrade* trade = GC.getGame().GetGameTrade();
						int expectRoutes = 0;
						for (uint i = 0; trade && i < trade->m_aTradeConnections.size(); ++i) if (!trade->IsTradeRouteIndexEmpty((int)i)) ++expectRoutes;
						if (r.Ask(inst, GC_Q_TRADE_ROUTES, all, 1) && r.h.record_size == 16) { routes = (int)r.h.count; if (routes != expectRoutes) ++artBad; }
						else ++artBad;
						// Q_TRADE_PATHS (B4 fix-path): each route's plots are its m_aPlotList in order (the trade route path finder's path, what the
						// caravan walks), from the origin city to the destination city, each step to a neighbour, and a land route crosses no mountain.
						{
							Reply p;
							int pathBad = 0, mountains = 0, tradePlots = 0, pathRoutes = 0;
							if (p.Ask(inst, GC_Q_TRADE_PATHS, all, 1) && p.h.record_size == sizeof(gcq_trade_plot))
							{
								uint32_t k = 0;
								for (uint i = 0; trade && i < trade->m_aTradeConnections.size(); ++i)
								{
									if (trade->IsTradeRouteIndexEmpty((int)i)) continue;
									const TradeConnection& route = trade->m_aTradeConnections[i];
									++pathRoutes;
									const uint n = route.m_aPlotList.size();
									if (n < 2 || route.m_aPlotList[0].m_iX != route.m_iOriginX || route.m_aPlotList[0].m_iY != route.m_iOriginY ||
										route.m_aPlotList[n - 1].m_iX != route.m_iDestX || route.m_aPlotList[n - 1].m_iY != route.m_iDestY) ++pathBad;
									for (uint j = 0; j < n; ++j, ++k)
									{
										if (k >= p.h.count) { ++pathBad; break; }
										gcq_trade_plot tp;
										memcpy(&tp, p.Rec(k), sizeof(tp));
										++tradePlots;
										if (tp.route != route.m_iID || tp.x != route.m_aPlotList[j].m_iX || tp.y != route.m_aPlotList[j].m_iY) ++pathBad;
										const CvPlot* at = GC.getMap().plot(tp.x, tp.y);
										const CvPlot* before = j > 0 ? GC.getMap().plot(route.m_aPlotList[j - 1].m_iX, route.m_aPlotList[j - 1].m_iY) : NULL;
										if (!at || (j > 0 && (!before || !before->isAdjacent(at)))) ++pathBad;
										else if (route.m_eDomain == DOMAIN_LAND && at->isMountain() && !at->isCity()) ++mountains;
									}
								}
								if (k != p.h.count) ++pathBad;
							}
							else ++pathBad;
							c.add("trade_paths", pathBad == 0 && mountains == 0, "Q_TRADE_PATHS: %d route(s), %d plot(s): %d bad, %d land plot(s) on a mountain", pathRoutes, tradePlots, pathBad, mountains);
						}
					}
					{
						// Q_CATALOG table 16 (the art technologies) and Q_PLAYERS.art_techs against the core's technologies.
						Reply cat;
						std::vector<int> techs;
						const int32_t table[1] = {GC_CATALOG_ART_TECHS};
						bool economics = false;
						if (cat.Ask(inst, GC_Q_CATALOG, table, 1) && cat.h.record_size == sizeof(gcq_catalog))
							for (uint32_t i = 0; i < cat.h.count; ++i)
							{
								gcq_catalog e;
								memcpy(&e, cat.Rec(i), sizeof(e));
								techs.push_back(e.id);
								if (e.type != GCQ_NO_STRING && strcmp((const char*)cat.buf.data + cat.h.strings_offset + e.type, "TECH_ECONOMICS") == 0) economics = true;
							}
						artTechCount = (int)techs.size();
						if (techs.empty() || techs.size() > 64 || !economics) ++playerBad;
						Reply pl;
						const int32_t all[1] = {-1};
						if (pl.Ask(inst, GC_Q_PLAYERS, all, 1) && pl.h.record_size >= 48)
							for (uint32_t i = 0; i < pl.h.count; ++i)
							{
								gcq_player p2;
								memcpy(&p2, pl.Rec(i), sizeof(p2));
								uint64_t want = 0;
								for (size_t b = 0; b < techs.size(); ++b)
									if (GET_TEAM((TeamTypes)p2.team).GetTeamTechs()->HasTech((TechTypes)techs[b])) want |= (uint64_t)1 << b;
								if (p2.art_techs != want) ++playerBad;
							}
						else ++playerBad;
					}
					{
						// WORKED: set on a worked plot for the whole truth and for the owner's team, never for a team that does not see the plot.
						Reply all, mine;
						const int32_t a[1] = {-1};
						CvPlot* wp = NULL;
						for (size_t i = 0; i < plotCount && !wp; ++i)
						{
							CvPlot* p = map.plotByIndex((int)i);
							if (p && p->isBeingWorked() && p->getOwner() != NO_PLAYER && !p->isCity()) wp = p;
						}
						if (wp)
						{
							const int idx = wp->getY() * map.getGridWidth() + wp->getX();
							const int32_t aq[2] = {-1, idx};
							const int32_t oq[2] = {(int32_t)GET_PLAYER(wp->getOwner()).getTeam(), idx};
							if (all.Ask(inst, GC_Q_PLOTS, aq, 2) && all.h.count == 1)
							{
								gcq_plot g;
								memcpy(&g, all.Rec(0), sizeof(g));
								if (!(g.flags & GC_PLOT_F_WORKED)) ++workedBad;
								++worked;
							}
							else ++workedBad;
							if (mine.Ask(inst, GC_Q_PLOTS, oq, 2) && mine.h.count == 1)
							{
								gcq_plot g;
								memcpy(&g, mine.Rec(0), sizeof(g));
								if ((g.flags & GC_PLOT_F_VISIBLE) && !(g.flags & GC_PLOT_F_WORKED)) ++workedBad;
								if (!(g.flags & GC_PLOT_F_VISIBLE) && (g.flags & GC_PLOT_F_WORKED)) ++workedBad;
							}
							else ++workedBad;
							for (size_t k = 0; k < teams.size(); ++k)
								if (teams[k] != (int)GET_PLAYER(wp->getOwner()).getTeam())
								{
									const int32_t xq[2] = {teams[k], idx};
									Reply other;
									if (other.Ask(inst, GC_Q_PLOTS, xq, 2) && other.h.count == 1)
									{
										gcq_plot g;
										memcpy(&g, other.Rec(0), sizeof(g));
										if (g.flags & GC_PLOT_F_WORKED) ++workedBad;  // owned by another team: never worked for it
									}
								}
						}
						(void)a;
					}
					{
						// Edits go through the dirty set: a build under way on a visible plot, a pillaged route.
						const BuildTypes farmBuild = (BuildTypes)GC.getInfoTypeForString("BUILD_FARM", true);
						const RouteTypes road = (RouteTypes)GC.getInfoTypeForString("ROUTE_ROAD", true);
						CvPlot* bp = NULL;
						CvPlot* rp = NULL;
						for (size_t i = 0; i < plotCount && !(bp && rp); ++i)
						{
							CvPlot* p = map.plotByIndex((int)i);
							if (!p || p->isWater() || p->isCity() || p->isMountain() || !p->isVisible((TeamTypes)t, false) || p->getImprovementType() != NO_IMPROVEMENT || p->getRouteType() != NO_ROUTE) continue;
							if (!bp) bp = p; else if (!rp) rp = p;
						}
						Reply settle;
						settle.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0);
						if (bp && farmBuild != NO_BUILD && rp && road != NO_ROUTE)
						{
							bp->changeBuildProgress(farmBuild, bp->getBuildTime(farmBuild, bp->getOwner()) / 2, bp->getOwner());  // half way
							rp->setRouteType(road);
							rp->SetRoutePillaged(true);
							Reply d;
							bool listedB = false, listedR = false;
							const int bi = bp->getY() * map.getGridWidth() + bp->getX(), ri = rp->getY() * map.getGridWidth() + rp->getX();
							if (d.Ask(inst, GC_Q_PLOTS_DIRTY, NULL, 0))
								for (uint32_t i = 0; i < d.h.count; ++i)
								{
									gcq_plot_dirty rec;
									memcpy(&rec, d.Rec(i), sizeof(rec));
									if ((int)rec.index == bi) listedB = true;
									if ((int)rec.index == ri) listedR = true;
								}
							Reply v;
							const int32_t bq[2] = {t, bi}, rq[2] = {t, ri};
							gcq_plot gb, gr;
							memset(&gb, 0, sizeof(gb));
							memset(&gr, 0, sizeof(gr));
							if (v.Ask(inst, GC_Q_PLOTS, bq, 2) && v.h.count == 1) memcpy(&gb, v.Rec(0), sizeof(gb));
							if (v.Ask(inst, GC_Q_PLOTS, rq, 2) && v.h.count == 1) memcpy(&gr, v.Rec(0), sizeof(gr));
							if (!listedB || !listedR || gb.build_type != (int16_t)farmBuild || gb.build_pct == 0 || !(gr.flags & GC_PLOT_F_ROUTE_PILLAGED) || gr.route != (int16_t)road) ++editBad;
							bad += Format(" edits: build plot %d listed %d type %d pct %u, route plot %d listed %d pillaged %d;", bi, listedB ? 1 : 0, (int)gb.build_type, (unsigned)gb.build_pct, ri, listedR ? 1 : 0, (gr.flags & GC_PLOT_F_ROUTE_PILLAGED) ? 1 : 0);
						}
						else { ++editBad; bad += " no plots for the edits;"; }
					}
					c.add("p13_fields", sizeBad == 0 && unitBad == 0 && cityBad == 0 && artBad == 0 && playerBad == 0 && workedBad == 0 && editBad == 0,
						"record sizes (Q_PLOTS 32, Q_UNITS 44, Q_CITIES 56, Q_CITY_ART 12, Q_TRADE_ROUTES 16): %d bad; %d unit(s) with facing / transport / build: %d bad; %d city(ies) with founder: %d bad; Q_CITY_ART %d record(s), Q_TRADE_ROUTES %d route(s): %d bad; art technologies %d (Q_CATALOG 16 and Q_PLAYERS.art_techs): %d bad; worked plots %d: %d bad; scenario-only edits (build progress, pillaged route) through the dirty set: %d bad;%s",
						sizeBad, units, unitBad, cities, cityBad, artRecords, routes, artBad, artTechCount, playerBad, worked, workedBad, editBad, bad.c_str());
				}
				// A bad index is INVALID_ARG, an empty basis request is the whole map.
				{
					Reply r;
					const int32_t bad1[1] = {(int32_t)plotCount}, bad2[2] = {-1, -1}, ok1[1] = {0};
					const bool a1 = !r.Ask(inst, GC_Q_PLOT_BASIS, bad1, 1) && r.rc == GC_ERR_INVALID_ARG;
					const bool a2 = !r.Ask(inst, GC_Q_PLOTS, bad2, 2) && r.rc == GC_ERR_INVALID_ARG;
					const bool a3 = r.Ask(inst, GC_Q_PLOT_BASIS, ok1, 1) && r.h.count == 1;
					const bool a4 = r.Ask(inst, GC_Q_PLOT_BASIS, NULL, 0) && r.h.count == plotCount;
					const bool a5 = !r.Ask(inst, GC_Q_PLOTS_DIRTY, ok1, 1) && r.rc == GC_ERR_INVALID_ARG;
					c.add("arguments", a1 && a2 && a3 && a4 && a5, "basis index out of range %d, Q_PLOTS index -1 %d, one index %d, no index = the whole map %d, Q_PLOTS_DIRTY with an argument %d (1 = as the spec says)", a1 ? 1 : 0, a2 ? 1 : 0, a3 ? 1 : 0, a4 ? 1 : 0, a5 ? 1 : 0);
				}
			}
		}
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("dirty-event|%s\n", seen.lines[i].c_str());

	const int comparedSteps = std::max(0, steps - 1);
	c.add("turns", doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) (turn %d -> %d), %d step(s), %d gc_pump call(s)%s%s", doneTurns, turns, startTurn, endTurn, steps, calls, stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0 && pumpErrors == 0, "%d STUCK, %d gc_pump error(s)", stuck, pumpErrors);
	c.add("no_misses", misses == 0 && comparedSteps > 0, "0 misses wanted: %d plot(s) changed in a Q_PLOTS or basis record without being in the dirty set, in %d of %d compared step(s)%s%s", misses, missPlots, comparedSteps,
		firstMiss.empty() ? "" : "; first: ", firstMiss.c_str());
	c.add("first_call_all", keyframeOk == 1 && keyframeBad == 0, "the first call listed every plot (%d ok, %d not)", keyframeOk, keyframeBad);
	c.add("reset_by_the_call", idleChecks > 0 && idleNonEmpty == 0, "a second call right after a call lists no plot (%d of %d checks non-empty)", idleNonEmpty, idleChecks);
	c.add("index_forms", indexChecks > 0 && indexBad == 0, "Q_PLOTS(team, indices) and Q_PLOT_BASIS(indices) of the dirty plots equal the rows of the full replies: %d check(s), %d bad%s%s", indexChecks, indexBad,
		indexFirstBad.empty() ? "" : "; first: ", indexFirstBad.c_str());
	c.add("basis_agrees", basisBad == 0, "Q_PLOT_BASIS agrees with Q_PLOTS(-1) on type, terrain, continent art, rivers, lake, coast, natural wonder: %d plot(s) differ%s%s", basisBad, basisFirstBad.empty() ? "" : "; first: ",
		basisFirstBad.c_str());
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);
	printf("dirty-cost|plots %u|teams %d|steps %d|Q_PLOTS_DIRTY p50 %.3f ms p95 %.3f ms max %.3f ms|full Q_PLOTS x teams + basis p50 %.3f ms|dirty per step %.1f of %u plots|false dirty %.1f per step|keyframe steps %d|snapshot %u bytes|rule 3 ms: %s\n",
		(unsigned)plotCount, teamCount, steps, Percentile(dirtyMs, 0.5), Percentile(dirtyMs, 0.95), Percentile(dirtyMs, 1.0), Percentile(fullMs, 0.5), steps ? (double)dirtySum / (double)steps : 0.0, (unsigned)plotCount,
		comparedSteps ? (double)falseDirty / (double)comparedSteps : 0.0, dirtyAllSteps, (unsigned)snapshotBytes, Percentile(dirtyMs, 0.95) > 3.0 ? "ABOVE -> conditional task D5" : "below");
	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) + ",\"turns_done\":" + Num(doneTurns) + ",\"steps\":" + Num(steps) +
		",\"plots\":" + Num((long long)plotCount) + ",\"teams\":" + Num(teamCount) + ",\"misses\":" + Num(misses) + ",\"dirty_ms\":{\"p50\":" + Format("%.3f", Percentile(dirtyMs, 0.5)) + ",\"p95\":" + Format("%.3f", Percentile(dirtyMs, 0.95)) +
		",\"max\":" + Format("%.3f", Percentile(dirtyMs, 1.0)) + "},\"full_ms_p50\":" + Format("%.3f", Percentile(fullMs, 0.5)) + ",\"dirty_per_step\":" + Format("%.1f", steps ? (double)dirtySum / (double)steps : 0.0) +
		",\"false_dirty_per_step\":" + Format("%.1f", comparedSteps ? (double)falseDirty / (double)comparedSteps : 0.0) + ",\"snapshot_bytes\":" + Num((long long)snapshotBytes) + ",\"total_ms\":" + Format("%.1f", NowMs() - t0) +
		",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("dirty-summary|seed %u|humans %u|ai %u|turns %d/%u|misses %d|stuck %d|log errors %d|%s\n", seed, humans, ai, doneTurns, turns, misses, stuck, seen.logErrors, stopReason.c_str());
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
