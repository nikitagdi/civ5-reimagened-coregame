// Scenarios `mp` and `mphybrid` (task N3.5; run.sh --scenario mp|mphybrid --humans N --ai A --turns T [--seeds S,..]): a network game
// (GAME_NETWORK_MULTIPLAYER, Pitboss, the server seat as active player) with N humans played by test bots, through the C ABI only, as the game server
// would drive it: gc_create, gc_new_game({"game_type":"network","players":[..],"turn_mode":..,"autoplay_humans":[0..N-1]}), gc_pump until the game turn is
// start + T. gc_pump hands every WAITING_HUMAN to the bots (gc_abi.cpp, one RtAutoplay per human slot) and reports AI_RUNNING, so the host only pumps.
//  mp        simultaneous mode, 2-4 humans + AI, 100 turns (default), seeds 1-3. Checks: all turns, 0 STUCK / GAME_OVER / gc_pump errors, 0 Lua and log
//            errors, the active player is the server seat after every call, every human ended every turn, humans_left follows the humans' ends of turn,
//            END_TURN then CANCEL_END_TURN of a waiting human (the turn waits for it again), then (last) the game ends when every human is dead
//            (decision 3.13): GC_PUMP_GAME_OVER and one GAME_OVER event. "mp-digest|turn|hex" lines (RtSession::StateDigest) let run.sh compare two runs of
//            a seed turn by turn.
//  mphybrid  hybrid mode: at game turn 5 human 0 declares war on human 1 (DECLARE_WAR through gc_apply_command); from the turn after, the two are never
//            turn-active together; a third human (peace with both) moves together with one of them; a command of the one waiting for its turn is
//            refused with GC_REASON_NOT_YOUR_TURN; peace made six turns after the war (MAKE_PEACE, N3.10) brings the simultaneous turn back.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "CvGameCoreUtils.h"
#include "rt_active.h"
#include "rt_autoplay.h"
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
	std::string prefix;
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
		printf("%s: %-4s %s: %s\n", prefix.c_str(), ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings;
	int events[32];
	int luaErrors;
	int endedSent, endedCancelled;     // event 12 by its flag
	int warEvents, warArgs[3];         // event 15: how many, and the first one's from_team, to_team, extends_timer
	// N3.7: events by player: the notifications each human got (the key of one is the pair player, id), his messages, popups.
	std::set<std::pair<int, int> > liveNotes;
	int notesAdded[MAX_PLAYERS], notesRemoved[MAX_PLAYERS], messages[MAX_PLAYERS], popups[MAX_PLAYERS], badShape;
	std::string lastMessage;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0), endedSent(0), endedCancelled(0), warEvents(0), badShape(0)
	{
		memset(events, 0, sizeof(events));
		memset(warArgs, 0, sizeof(warArgs));
		memset(notesAdded, 0, sizeof(notesAdded));
		memset(notesRemoved, 0, sizeof(notesRemoved));
		memset(messages, 0, sizeof(messages));
		memset(popups, 0, sizeof(popups));
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
	if (ev->kind >= 0 && ev->kind < 32) ++s->events[ev->kind];
	if (ev->kind == GC_EVENT_PLAYER_TURN_ENDED && ev->argc >= 2) { if (ev->args[1]) ++s->endedSent; else ++s->endedCancelled; }
	if (ev->kind == GC_EVENT_HUMAN_WAR_DECLARED && ev->argc >= 3)
	{
		if (s->warEvents == 0) for (int i = 0; i < 3; ++i) s->warArgs[i] = ev->args[i];
		++s->warEvents;
	}
	if (ev->kind == GC_EVENT_NOTIFICATION_ADDED)
	{
		if (ev->argc != 7 || ev->args[6] < 0 || ev->args[6] >= MAX_PLAYERS || !ev->text || !ev->text2) ++s->badShape;
		else
		{
			++s->notesAdded[ev->args[6]];
			s->liveNotes.insert(std::make_pair(ev->args[6], ev->args[0]));
		}
	}
	if (ev->kind == GC_EVENT_NOTIFICATION_REMOVED)
	{
		if (ev->argc != 2 || ev->args[1] < 0 || ev->args[1] >= MAX_PLAYERS) ++s->badShape;
		else
		{
			++s->notesRemoved[ev->args[1]];
			s->liveNotes.erase(std::make_pair(ev->args[1], ev->args[0]));
		}
	}
	if (ev->kind == GC_EVENT_MESSAGE)
	{
		if (ev->argc != 6 || ev->args[0] < 0 || ev->args[0] >= MAX_PLAYERS || !ev->text) ++s->badShape;
		else
		{
			++s->messages[ev->args[0]];
			s->lastMessage = ev->text;
		}
	}
	if (ev->kind == GC_EVENT_POPUP_INFO)
	{
		if (ev->argc != 5 || ev->args[4] < 0 || ev->args[4] >= MAX_PLAYERS) ++s->badShape;
		else ++s->popups[ev->args[4]];
	}
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

// ---- N3.7 / N3.9: the per-player queries at the end of the run (audit F-05, F-06) ----

// A gc_query reply with its header; released by the destructor.
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
		return h.kind == kind && h.record_size > 0;
	}
	template <class T>
	T At(uint32_t i) const
	{
		T r;
		memset(&r, 0, sizeof(r));
		memcpy(&r, buf.data + sizeof(h) + (size_t)i * h.record_size, sizeof(T) < h.record_size ? sizeof(T) : h.record_size);
		return r;
	}
	const char* Str(uint32_t off) const { return off == GCQ_NO_STRING ? "" : (const char*)buf.data + h.strings_offset + off; }
};

struct Probes
{
	bool ran;
	int humansAsked, noteMismatch, noteAiRecords, noteSeatRecords, noteTotal;
	std::string noteFirst;
	int barFail, barAsked, buildsAsked, buildsFail, otherBarFail;
	std::string barFirst;
	int digestFail;
	std::string digestNote;
	int treeUnits, treeTrees, treeNodes, treeFail, treeSamples, treeOrphans, reachFail, unreachAsked, unreachOk;
	double treeMs[4];
	int treeCalls[4];
	std::string treeFirst;
	int previewAsked, previewFail, previewPure;
	bool queriesPure;
	int msgPlayer, msgBefore, msgAfter, msgOthers;
	std::string msgNote;
	std::string previewNote;
	Probes() : ran(false), humansAsked(0), noteMismatch(0), noteAiRecords(0), noteSeatRecords(0), noteTotal(0), barFail(0), barAsked(0), buildsAsked(0), buildsFail(0), otherBarFail(0), digestFail(0),
		treeUnits(0), treeTrees(0), treeNodes(0), treeFail(0), treeSamples(0), treeOrphans(0), reachFail(0), unreachAsked(0), unreachOk(0), previewAsked(0), previewFail(0), previewPure(0), queriesPure(false), msgPlayer(-1), msgBefore(0), msgAfter(0), msgOthers(0)
	{
		for (int i = 0; i < 4; ++i) { treeMs[i] = 0; treeCalls[i] = 0; }
	}
};

// An empty, dry, passable plot without a city or a goody hut next to (x, y), not (avoidX, avoidY); NULL when there is none.
CvPlot* FreeLandNextTo(int x, int y, int avoidX, int avoidY)
{
	for (int d = 0; d < NUM_DIRECTION_TYPES; ++d)
	{
		CvPlot* p = plotDirection(x, y, (DirectionTypes)d);
		if (!p || p->isWater() || p->isImpassable() || p->isMountain() || p->isCity() || p->isGoody() || p->getNumUnits() != 0) continue;
		if (p->getX() == avoidX && p->getY() == avoidY) continue;
		return p;
	}
	return NULL;
}

struct NodeKey
{
	int x, y;
	bool operator<(const NodeKey& o) const { return x != o.x ? x < o.x : y < o.y; }
};

bool Digest2(gc_instance* inst, uint64_t* out)
{
	gc_error e;
	memset(&e, 0, sizeof(e));
	return gc_state_digest(inst, -2, out, &e) == GC_OK;
}

// Q_NOTIFICATIONS of every human = the events' live set for him; an AI and the seat have none. Q_TOP_BAR and Q_CITY_BUILDS of every human (and of an AI: any living major).
void ProbeNotesAndBars(gc_instance* inst, const Seen& seen, const std::vector<int>& humans, int seat, Probes& pr)
{
	Reply r;
	for (size_t k = 0; k < humans.size(); ++k)
	{
		const int h = humans[k];
		const int32_t a[1] = {(int32_t)h};
		++pr.humansAsked;
		if (!r.Ask(inst, GC_Q_NOTIFICATIONS, a, 1)) { ++pr.noteMismatch; if (pr.noteFirst.empty()) pr.noteFirst = Format("Q_NOTIFICATIONS(%d): rc %d %s", h, r.rc, r.err.c_str()); continue; }
		std::set<int> ids;
		for (uint32_t i = 0; i < r.h.count; ++i)
		{
			const gcq_notification n = r.At<gcq_notification>(i);
			ids.insert(n.id);
			if (!r.Str(n.message)[0] || !r.Str(n.summary)[0]) { ++pr.noteMismatch; if (pr.noteFirst.empty()) pr.noteFirst = Format("player %d note %d without text", h, n.id); }
		}
		std::set<int> want;
		for (std::set<std::pair<int, int> >::const_iterator it = seen.liveNotes.begin(); it != seen.liveNotes.end(); ++it)
			if (it->first == h) want.insert(it->second);
		pr.noteTotal += (int)ids.size();
		if (ids != want)
		{
			++pr.noteMismatch;
			if (pr.noteFirst.empty()) pr.noteFirst = Format("player %d: Q_NOTIFICATIONS lists %d, the events leave %d live", h, (int)ids.size(), (int)want.size());
		}
	}
	// An AI major and the server seat: no list.
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isAlive() || IsSeatedHuman(i)) continue;
		const int32_t a[1] = {(int32_t)i};
		if (r.Ask(inst, GC_Q_NOTIFICATIONS, a, 1) && r.h.count > 0) { if (i == seat) ++pr.noteSeatRecords; else ++pr.noteAiRecords; }
	}
	// Top bars and city builds.
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isAlive()) continue;
		const int32_t a[1] = {(int32_t)i};
		const bool human = IsSeatedHuman(i);
		++pr.barAsked;
		if (i == seat)
		{
			if (r.Ask(inst, GC_Q_TOP_BAR, a, 1) || r.rc != GC_ERR_INVALID_ARG) ++pr.otherBarFail;
			continue;
		}
		if (!r.Ask(inst, GC_Q_TOP_BAR, a, 1) || r.h.count != 1)
		{
			if (human) ++pr.barFail; else ++pr.otherBarFail;
			if (pr.barFirst.empty()) pr.barFirst = Format("Q_TOP_BAR(%d): rc %d %s", i, r.rc, r.err.c_str());
			continue;
		}
		const gcq_top_bar tb = r.At<gcq_top_bar>(0);
		if (tb.gold != p.GetTreasury()->GetGold() || tb.era != (int)p.GetCurrentEra()) { ++pr.barFail; if (pr.barFirst.empty()) pr.barFirst = Format("Q_TOP_BAR(%d): gold %d vs %d", i, (int)tb.gold, (int)p.GetTreasury()->GetGold()); }
		int loop;
		CvCity* city = const_cast<CvPlayer&>(p).firstCity(&loop);
		if (city)
		{
			const int32_t b[2] = {(int32_t)i, (int32_t)city->GetID()};
			++pr.buildsAsked;
			if (!r.Ask(inst, GC_Q_CITY_BUILDS, b, 2) || r.h.count == 0) { ++pr.buildsFail; if (pr.barFirst.empty()) pr.barFirst = Format("Q_CITY_BUILDS(%d, city %d): rc %d %s", i, city->GetID(), r.rc, r.err.c_str()); }
		}
	}
}

// Q_REACHABLE_TREE against Q_PATH and Q_REACHABLE for the first movable units of every human (N3.9).
void ProbeTrees(gc_instance* inst, const std::vector<int>& humans, Probes& pr)
{
	unsigned rnd = 12345;
	for (size_t k = 0; k < humans.size(); ++k)
	{
		CvPlayer& p = GET_PLAYER((PlayerTypes)humans[k]);
		int loop, used = 0;
		for (CvUnit* u = p.firstUnit(&loop); u && used < 3; u = p.nextUnit(&loop))
		{
			if (u->isDelayedDeath() || u->IsBusy() || !u->canMove() || u->IsImmobile()) continue;
			++used;
			++pr.treeUnits;
			const int32_t base[2] = {(int32_t)humans[k], (int32_t)u->GetID()};
			std::map<NodeKey, gcq_tree_node> prev;  // the tree of the previous (smaller) horizon
			for (int hz = 1; hz <= 3; ++hz)
			{
				const int32_t a[3] = {base[0], base[1], hz};
				Reply t;
				const double t0 = NowMs();
				const bool ok = t.Ask(inst, GC_Q_REACHABLE_TREE, a, 3);
				pr.treeMs[hz] += NowMs() - t0;
				++pr.treeCalls[hz];
				if (!ok || t.h.record_size < sizeof(gcq_tree_node))
				{
					++pr.treeFail;
					if (pr.treeFirst.empty()) pr.treeFirst = Format("Q_REACHABLE_TREE(player %d, unit %d, horizon %d): rc %d %s", humans[k], (int)u->GetID(), hz, t.rc, t.err.c_str());
					continue;
				}
				++pr.treeTrees;
				pr.treeNodes += (int)t.h.count;
				std::map<NodeKey, gcq_tree_node> tree;
				for (uint32_t i = 0; i < t.h.count; ++i)
				{
					const gcq_tree_node n = t.At<gcq_tree_node>(i);
					NodeKey key = {n.x, n.y};
					tree[key] = n;
					if (n.turn >= hz || (n.x == u->getX() && n.y == u->getY()))
					{
						++pr.treeFail;
						if (pr.treeFirst.empty()) pr.treeFirst = Format("horizon %d: node (%d,%d) turn %d", hz, (int)n.x, (int)n.y, (int)n.turn);
					}
				}
				// A larger horizon keeps every node of the smaller one.
				for (std::map<NodeKey, gcq_tree_node>::const_iterator it = prev.begin(); it != prev.end(); ++it)
					if (tree.find(it->first) == tree.end())
					{
						++pr.treeFail;
						if (pr.treeFirst.empty()) pr.treeFirst = Format("horizon %d lost node (%d,%d) of horizon %d", hz, it->first.x, it->first.y, hz - 1);
						break;
					}
				prev = tree;
				if (hz == 1)
				{
					// Horizon 1 is Q_REACHABLE: same plots, same turn and moves.
					Reply q;
					if (!q.Ask(inst, GC_Q_REACHABLE, base, 2) || q.h.count != tree.size()) ++pr.reachFail;
					else
						for (uint32_t i = 0; i < q.h.count; ++i)
						{
							const gcq_path_node n = q.At<gcq_path_node>(i);
							NodeKey key = {n.x, n.y};
							std::map<NodeKey, gcq_tree_node>::const_iterator f = tree.find(key);
							if (f == tree.end() || f->second.moves_left != n.moves_left || f->second.turn != n.turn || f->second.flags != n.flags) { ++pr.reachFail; break; }
						}
				}
				// Samples: Q_PATH to a node ends with that node (turn, moves, flags) and the step before it is the parent; walking the parents reaches the unit.
				std::vector<gcq_tree_node> all;
				for (std::map<NodeKey, gcq_tree_node>::const_iterator it = tree.begin(); it != tree.end(); ++it) all.push_back(it->second);
				for (int s2 = 0; s2 < 10 && !all.empty(); ++s2)
				{
					rnd = rnd * 1103515245u + 12345u;
					const gcq_tree_node n = all[(rnd >> 8) % all.size()];
					const int32_t pa[4] = {base[0], base[1], (int32_t)n.x, (int32_t)n.y};
					Reply pth;
					++pr.treeSamples;
					bool bad = !pth.Ask(inst, GC_Q_PATH, pa, 4) || pth.h.count < 2;
					if (!bad)
					{
						const gcq_path_node last = pth.At<gcq_path_node>(pth.h.count - 1), before = pth.At<gcq_path_node>(pth.h.count - 2);
						bad = last.x != n.x || last.y != n.y || last.turn != n.turn || last.moves_left != n.moves_left || last.flags != n.flags || before.x != n.parent_x || before.y != n.parent_y ||
							(last.flags & GC_REACH_F_UNREACHABLE) != 0;
					}
					// The parents lead to the unit's plot, the turns never grow towards the root.
					gcq_tree_node cur = n;
					for (int steps = 0; !bad && steps < 80; ++steps)
					{
						if (cur.parent_x == u->getX() && cur.parent_y == u->getY()) break;
						NodeKey pk = {cur.parent_x, cur.parent_y};
						std::map<NodeKey, gcq_tree_node>::const_iterator f = tree.find(pk);
						if (f == tree.end()) { ++pr.treeOrphans; break; }
						if (f->second.turn > cur.turn) bad = true;
						cur = f->second;
					}
					if (bad)
					{
						++pr.treeFail;
						if (pr.treeFirst.empty()) pr.treeFirst = Format("player %d unit %d horizon %d: node (%d,%d) turn %d moves %d vs Q_PATH rc %d count %u", humans[k], (int)u->GetID(), hz, (int)n.x, (int)n.y, (int)n.turn,
							(int)n.moves_left, pth.rc, (unsigned)pth.h.count);
					}
				}
			}
			// A target the unit cannot reach: one node, the target, UNREACHABLE (a mountain for a land unit, or any plot the search refuses).
			if (u->getDomainType() == DOMAIN_LAND)
			{
				CvMap& map = GC.getMap();
				for (int y = 0; y < map.getGridHeight(); ++y)
				{
					bool done = false;
					for (int x = 0; x < map.getGridWidth() && !done; ++x)
					{
						const CvPlot* plot = map.plot(x, y);
						if (!plot || !plot->isMountain() || !plot->isRevealed(p.getTeam(), false)) continue;
						const int32_t pa[4] = {base[0], base[1], x, y};
						Reply pth;
						if (!pth.Ask(inst, GC_Q_PATH, pa, 4)) continue;
						if (pth.h.count == 1 && (pth.At<gcq_path_node>(0).flags & GC_REACH_F_UNREACHABLE)) ++pr.unreachOk;
						if (pth.h.count == 1) { ++pr.unreachAsked; done = true; }
					}
					if (done) break;
				}
			}
		}
	}
}

// Q_COMBAT_PREVIEW: a unit and a city of the first human with a barbarian next to them (spawned here, removed after): one record with strengths and damage, no side effect (the
// simulation digest and the game RNG stay), nothing for an unseen plot or an own unit.
void ProbePreview(gc_instance* inst, const std::vector<int>& humans, Probes& pr)
{
	const UnitTypes warriorType = (UnitTypes)GC.getInfoTypeForString("UNIT_WARRIOR", true);
	const UnitTypes archerType = (UnitTypes)GC.getInfoTypeForString("UNIT_ARCHER", true);
	if (warriorType == NO_UNIT || archerType == NO_UNIT || humans.empty()) { pr.previewNote = "no UNIT_WARRIOR / UNIT_ARCHER or no human"; return; }
	for (size_t k = 0; k < humans.size() && pr.previewAsked < 3; ++k)
	{
		const int h = humans[k];
		CvPlayer& p = GET_PLAYER((PlayerTypes)h);
		CvUnit* melee = NULL;
		CvPlot* at = NULL;
		int loop;
		for (CvUnit* u = p.firstUnit(&loop); u && !at; u = p.nextUnit(&loop))
		{
			if (u->isDelayedDeath() || !u->IsCombatUnit() || u->IsCanAttackRanged() || u->getDomainType() != DOMAIN_LAND) continue;
			at = FreeLandNextTo(u->getX(), u->getY(), -1, -1);
			if (at) melee = u;
		}
		if (!melee || !at) continue;
		CvUnit* barb = GET_PLAYER(BARBARIAN_PLAYER).initUnit(warriorType, at->getX(), at->getY(), NO_UNITAI, NO_DIRECTION, false, false);
		if (!barb) continue;
		const int barbId = barb->GetID();
		const int bx = barb->getX(), by = barb->getY();
		// Everything the previews look at is placed first: the archer of the human next to its melee unit, a second barbarian next to the human's first city.
		CvUnit* archer = p.initUnit(archerType, melee->getX(), melee->getY(), NO_UNITAI, NO_DIRECTION, false, false);
		int cityLoop;
		CvCity* city = p.firstCity(&cityLoop);
		CvUnit* barb2 = NULL;
		if (city)
		{
			CvPlot* cat = FreeLandNextTo(city->getX(), city->getY(), -1, -1);
			if (cat) barb2 = GET_PLAYER(BARBARIAN_PLAYER).initUnit(warriorType, cat->getX(), cat->getY(), NO_UNITAI, NO_DIRECTION, false, false);
		}
		++pr.previewAsked;
		uint64_t d0 = 0, d1 = 0;
		const bool haveD = Digest2(inst, &d0);
		const unsigned long rng0 = GC.getGame().getJonRand().getCallCount();
		std::string bad;
		{
			const int32_t a[4] = {(int32_t)h, (int32_t)melee->GetID(), bx, by};
			Reply r;
			if (!r.Ask(inst, GC_Q_COMBAT_PREVIEW, a, 4) || r.h.count != 1) bad += Format(" melee: rc %d count %u %s;", r.rc, (unsigned)r.h.count, r.err.c_str());
			else
			{
				const gcq_combat_preview cp = r.At<gcq_combat_preview>(0);
				if (cp.attacker_strength <= 0 || cp.defender_strength <= 0 || cp.damage_to_target <= 0 || cp.damage_to_target > 100 || cp.damage_to_attacker < 0 || cp.target_max_hp <= 0 ||
					cp.target_hp != cp.target_max_hp || (cp.flags & GC_COMBAT_F_CITY))
					bad += Format(" melee: damage %d / %d, strength %d vs %d, hp %d/%d, flags %x;", (int)cp.damage_to_target, (int)cp.damage_to_attacker, (int)cp.attacker_strength, (int)cp.defender_strength,
						(int)cp.target_hp, (int)cp.target_max_hp, (unsigned)cp.flags);
				pr.previewNote += Format("%smelee unit %d (%s) vs barbarian warrior: damage %d / %d, strength %d vs %d", pr.previewNote.empty() ? "" : "; ", (int)melee->GetID(), melee->getUnitInfo().GetType(),
					(int)cp.damage_to_target, (int)cp.damage_to_attacker, (int)cp.attacker_strength, (int)cp.defender_strength);
			}
			// An own plot, and a plot the human does not see: no record.
			const int32_t own[4] = {(int32_t)h, (int32_t)melee->GetID(), melee->getX(), melee->getY()};
			if (!r.Ask(inst, GC_Q_COMBAT_PREVIEW, own, 4) || r.h.count != 0) bad += " own plot gave a record;";
			CvMap& map = GC.getMap();
			for (int i = 0; i < map.numPlots(); ++i)
			{
				const CvPlot* plot = map.plotByIndex(i);
				if (plot && !plot->isVisible(p.getTeam(), false))
				{
					const int32_t fog[4] = {(int32_t)h, (int32_t)melee->GetID(), plot->getX(), plot->getY()};
					if (!r.Ask(inst, GC_Q_COMBAT_PREVIEW, fog, 4) || r.h.count != 0) bad += " an unseen plot gave a record;";
					break;
				}
			}
			const int32_t badUnit[4] = {(int32_t)h, 999999, bx, by};
			if (r.Ask(inst, GC_Q_COMBAT_PREVIEW, badUnit, 4) || r.rc != GC_ERR_INVALID_ARG) bad += " a missing unit was not INVALID_ARG;";
		}
		// A ranged unit of the human next to the barbarian, and the first city when the second barbarian is next to it.
		if (archer)
		{
			const int32_t a[4] = {(int32_t)h, (int32_t)archer->GetID(), bx, by};
			Reply r;
			if (!r.Ask(inst, GC_Q_COMBAT_PREVIEW, a, 4) || r.h.count != 1) bad += Format(" ranged: rc %d count %u %s;", r.rc, (unsigned)r.h.count, r.err.c_str());
			else
			{
				const gcq_combat_preview cp = r.At<gcq_combat_preview>(0);
				if (cp.damage_to_target <= 0 || cp.attacker_strength <= 0 || cp.defender_strength <= 0) bad += " ranged: no damage or strength;";
				pr.previewNote += Format("; archer vs the same: damage %d / %d, strength %d vs %d", (int)cp.damage_to_target, (int)cp.damage_to_attacker, (int)cp.attacker_strength, (int)cp.defender_strength);
			}
		}
		if (city)
		{
			if (barb2)
			{
				const int32_t a[4] = {(int32_t)h, (int32_t)-city->GetID(), barb2->getX(), barb2->getY()};
				Reply r;
				if (!r.Ask(inst, GC_Q_COMBAT_PREVIEW, a, 4) || r.h.count > 1) bad += Format(" city: rc %d count %u %s;", r.rc, (unsigned)r.h.count, r.err.c_str());
				else if (r.h.count == 1)
				{
					const gcq_combat_preview cp = r.At<gcq_combat_preview>(0);
					if (cp.damage_to_target <= 0 || cp.attacker_strength <= 0) bad += " city: no damage or strength;";
					pr.previewNote += Format("; city %d vs the barbarian next to it: damage %d, strength %d vs %d", city->GetID(), (int)cp.damage_to_target, (int)cp.attacker_strength, (int)cp.defender_strength);
				}
				else pr.previewNote += Format("; city %d: no record (cannot shoot now)", city->GetID());
			}
		}
		const unsigned long rng1 = GC.getGame().getJonRand().getCallCount();
		if (haveD && Digest2(inst, &d1) && d0 == d1 && rng0 == rng1) ++pr.previewPure;
		else bad += Format(" the previews changed the game (digest %016llx -> %016llx, rng %lu -> %lu);", (unsigned long long)d0, (unsigned long long)d1, rng0, rng1);
		if (!bad.empty())
		{
			++pr.previewFail;
			pr.previewNote += " | FAIL:" + bad;
		}
		// Clean up: the spawned units go.
		if (CvUnit* b = GET_PLAYER(BARBARIAN_PLAYER).getUnit(barbId)) b->kill(false);
		if (barb2) barb2->kill(false);
		if (archer) archer->kill(false);
	}
}

// Audit F-05: a fight a human orders gives him MESSAGE events (a network game used to drop every one of them: the filter passed the active player only, and that is the
// server seat). The archer of the first human with room shoots a barbarian next to it through the command path; the messages of that human grow, nobody else's do.
void ProbeMessages(gc_instance* inst, const Seen& seen, const std::vector<int>& humans, Probes& pr)
{
	const UnitTypes warriorType = (UnitTypes)GC.getInfoTypeForString("UNIT_WARRIOR", true);
	const UnitTypes archerType = (UnitTypes)GC.getInfoTypeForString("UNIT_ARCHER", true);
	if (warriorType == NO_UNIT || archerType == NO_UNIT) { pr.msgNote = "no UNIT_WARRIOR / UNIT_ARCHER"; return; }
	for (size_t k = 0; k < humans.size(); ++k)
	{
		CvPlayer& p = GET_PLAYER((PlayerTypes)humans[k]);
		CvPlot* archerAt = NULL;
		CvPlot* barbAt = NULL;
		int loop;
		for (CvUnit* u = p.firstUnit(&loop); u && !barbAt; u = p.nextUnit(&loop))
		{
			if (u->isDelayedDeath() || u->getDomainType() != DOMAIN_LAND) continue;
			archerAt = FreeLandNextTo(u->getX(), u->getY(), -1, -1);
			if (archerAt) barbAt = FreeLandNextTo(archerAt->getX(), archerAt->getY(), u->getX(), u->getY());
		}
		if (!archerAt || !barbAt) continue;
		CvUnit* archer = p.initUnit(archerType, archerAt->getX(), archerAt->getY(), NO_UNITAI, NO_DIRECTION, false, false);
		CvUnit* barb = GET_PLAYER(BARBARIAN_PLAYER).initUnit(warriorType, barbAt->getX(), barbAt->getY(), NO_UNITAI, NO_DIRECTION, false, false);
		if (!archer || !barb) continue;
		const int32_t args[3] = {(int32_t)archer->GetID(), (int32_t)barb->getX(), (int32_t)barb->getY()};
		const int barbId = barb->GetID();
		pr.msgPlayer = humans[k];
		pr.msgBefore = seen.messages[humans[k]];
		int others = 0;
		for (int i = 0; i < MAX_PLAYERS; ++i) if (i != humans[k]) others += seen.messages[i];
		gc_command cmd = Cmd(GC_CMD_UNIT_RANGE_ATTACK, humans[k], args, 3);
		gc_error err;
		memset(&err, 0, sizeof(err));
		const int32_t rc = gc_apply_command(inst, &cmd, &err);
		pr.msgNote = Format("player %d: UNIT_RANGE_ATTACK by Archer %d at (%d,%d): rc %d %s", humans[k], (int)archer->GetID(), args[1], args[2], rc, err.message);
		gc_pump_result pump;
		memset(&pump, 0, sizeof(pump));
		for (int n = 0; rc == GC_OK && n < 20 && seen.messages[humans[k]] == pr.msgBefore; ++n)
			if (gc_pump(inst, 64, &pump, &err) != GC_OK) break;
		pr.msgAfter = seen.messages[humans[k]];
		int othersAfter = 0;
		for (int i = 0; i < MAX_PLAYERS; ++i) if (i != humans[k]) othersAfter += seen.messages[i];
		pr.msgOthers = othersAfter - others;
		pr.msgNote += Format("; MESSAGE events for the human %d -> %d, for everybody else +%d; the first: \"%s\"", pr.msgBefore, pr.msgAfter, pr.msgOthers, seen.lastMessage.c_str());
		if (CvUnit* b = GET_PLAYER(BARBARIAN_PLAYER).getUnit(barbId)) b->kill(false);
		if (CvUnit* a = p.getUnit(args[0])) a->kill(false);
		return;
	}
	pr.msgNote = "no human unit with room for an archer and a barbarian";
}

// The simulation digest (viewer -2): stable between calls, and the team digests of the humans exist; the queries above must not have changed it.
void ProbeDigests(gc_instance* inst, const std::vector<int>& humans, Probes& pr)
{
	uint64_t a = 0, b = 0;
	gc_error e;
	memset(&e, 0, sizeof(e));
	if (!Digest2(inst, &a) || !Digest2(inst, &b) || a != b || a == 0) { ++pr.digestFail; pr.digestNote += " viewer -2 unstable;"; }
	std::set<uint64_t> teamDigests;
	for (size_t k = 0; k < humans.size(); ++k)
	{
		uint64_t d = 0;
		const int team = (int)GET_PLAYER((PlayerTypes)humans[k]).getTeam();
		if (gc_state_digest(inst, team, &d, &e) != GC_OK) { ++pr.digestFail; pr.digestNote += Format(" team %d digest rc %d;", team, e.code); continue; }
		teamDigests.insert(d);
	}
	if (humans.size() > 1 && teamDigests.size() < 2) { ++pr.digestFail; pr.digestNote += " all humans see the same digest;"; }
	pr.digestNote += Format(" simulation digest %016llx, %d distinct team digest(s) for %d human(s)", (unsigned long long)a, (int)teamDigests.size(), (int)humans.size());
}

}  // namespace

extern "C" int rt_selftest_mp(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len)
{
	const std::string turnMode = mode && *mode ? mode : "simultaneous";
	const bool hybrid = turnMode == "hybrid";
	Check c;
	c.prefix = hybrid ? "mphybrid" : "mp";
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const double t0 = NowMs();

	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	int startTurn = -1, endTurn = -1, doneTurns = 0, stuck = 0, gameOver = 0, pumpErrors = 0, calls = 0, humanPhaseTurns = 0;
	int activeRuleBroken = 0, humansLeftWrong = 0, humansLeftMax = 0, seat = -1;
	int bothWarringActive = 0, warringSteps = 0, warTurn = -1, peaceTurn = -1, bothAfterPeace = 0, thirdWithOne = 0, notYourTurn = 0, notYourTurnOk = 0;
	int cancelRc = -99, cancelReason = -1, endRc = -99, cancelTurn = -1, cancelOk = 0;
	int finalGameOverEvents = 0, finalStatus = -1, finalAlive = -1, turnEventsAtEnd = 0;
	int queryChecks = 0, queryBad = 0, qturnCountBad = 0;
	Probes probes;
	std::string queryFirstBad;
	std::string stopReason, setup, warHow, peaceHow, notYourTurnFirst, cancelNote, deadNote;
	std::vector<double> turnMs;
	std::vector<int> humanList;
	if (inst)
	{
		std::string players;
		for (unsigned i = 0; i < humans; ++i)
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		for (unsigned i = 0; i < ai; ++i) players += std::string(humans || i ? "," : "") + "{\"kind\":\"ai\",\"civ\":\"random\"}";
		std::string slots;
		for (unsigned i = 0; i < humans; ++i) slots += Format("%s%u", i ? "," : "", i);
		setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"%s\",\"autoplay_humans\":[%s],\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed, players.c_str(), turnMode.c_str(), slots.c_str(),
			world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		const double t1 = NowMs();
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s (%.0f ms)", setup.c_str(), rc, err.message, NowMs() - t1);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			CvGame& game = GC.getGame();
			seat = (int)ServerSeat();
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (IsSeatedHuman(i)) humanList.push_back(i);
			c.add("network_setup", game.isNetworkMultiPlayer() && CvPreGame::isPitBoss() && seat >= 0 && CvPreGame::activePlayer() == seat && humanList.size() == humans &&
				game.isOption(GAMEOPTION_DYNAMIC_TURNS) == hybrid && game.isOption(GAMEOPTION_SIMULTANEOUS_TURNS) == !hybrid,
				"game type %d, pitboss %d, simultaneous %d, dynamic %d, server seat %d, active %d, %d human(s)", (int)CvPreGame::gameType(), CvPreGame::isPitBoss() ? 1 : 0,
				game.isOption(GAMEOPTION_SIMULTANEOUS_TURNS) ? 1 : 0, game.isOption(GAMEOPTION_DYNAMIC_TURNS) ? 1 : 0, seat, (int)CvPreGame::activePlayer(), (int)humanList.size());

			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = game.getGameTurn();
			int turn = startTurn;
			const int target = startTurn + (int)turns;
			const long long maxCalls = 20000LL * (turns + 1);
			double tTurn = NowMs();
			int maxLeftThisTurn = 0;
			bool probeDone = false;
			while (turn < target)
			{
				if (++calls > maxCalls) { stopReason = Format("turn %d: %lld gc_pump calls without reaching turn %d", turn, maxCalls, target); break; }
				const int32_t prc = gc_pump(inst, 64, &pr, &err);
				if (CvPreGame::activePlayer() != seat) ++activeRuleBroken;
				if (pr.status == GC_PUMP_STUCK) { ++stuck; stopReason = Format("STUCK at turn %d: %s", pr.turn, err.message); break; }
				if (pr.status == GC_PUMP_GAME_OVER) { ++gameOver; stopReason = Format("GAME_OVER at turn %d", pr.turn); break; }
				if (prc != GC_OK) { ++pumpErrors; stopReason = Format("gc_pump at turn %d: %d %s", pr.turn, prc, err.message); break; }

				// humans_left = the living seated humans that have not ended the turn, in the human phase (0 while the AI civs move).
				int expectLeft = 0;
				if (session->HumanPhase())
					for (size_t h = 0; h < humanList.size(); ++h)
						if (GET_PLAYER((PlayerTypes)humanList[h]).isAlive() && !session->Engine().TurnCompleteSent((PlayerTypes)humanList[h])) ++expectLeft;
				if (pr.humans_left != expectLeft || pr.humans_left < 0 || pr.humans_left > (int)humans) ++humansLeftWrong;
				if (pr.humans_left > maxLeftThisTurn) maxLeftThisTurn = pr.humans_left;
				if (pr.humans_left > humansLeftMax) humansLeftMax = pr.humans_left;

				if (pr.turn != turn)
				{
					const double now = NowMs();
					turnMs.push_back(now - tTurn);
					tTurn = now;
					printf("%s-digest|%d|%016llx\n", c.prefix.c_str(), pr.turn, (unsigned long long)RtSession::StateDigest());
					if (turn % 10 == 0 || turn >= 80)
					{
						std::string st;
						for (size_t h = 0; h < humanList.size(); ++h)
						{
							const CvPlayer& hp = GET_PLAYER((PlayerTypes)humanList[h]);
							st += Format("%sp%d alive %d cities %d units %d war-with", h ? "; " : "", humanList[h], hp.isAlive() ? 1 : 0, hp.getNumCities(), hp.getNumUnits());
							for (int o = 0; o < MAX_MAJOR_CIVS; ++o)
								if (o != humanList[h] && GET_PLAYER((PlayerTypes)o).isAlive() && GET_TEAM(hp.getTeam()).isAtWar(GET_PLAYER((PlayerTypes)o).getTeam())) st += Format(" %d", o);
						}
						printf("%s-status|turn %d|%s\n", c.prefix.c_str(), pr.turn, st.c_str());
					}
					if (maxLeftThisTurn > 0) ++humanPhaseTurns;
					maxLeftThisTurn = 0;
					doneTurns += pr.turn - turn;
					turn = pr.turn;
					if (turn >= target) break;
				}

				// Hybrid: human 0 declares war on human 1 at game turn 5 through the command path.
				if (hybrid && warTurn < 0 && turn >= 5 && humanList.size() >= 2 && session->IsHumanWaiting((PlayerTypes)humanList[0]))
				{
					const TeamTypes t0t = GET_PLAYER((PlayerTypes)humanList[0]).getTeam(), t1t = GET_PLAYER((PlayerTypes)humanList[1]).getTeam();
					const bool met = GET_TEAM(t0t).isHasMet(t1t);
					if (!met) GET_TEAM(t0t).meet(t1t, true);  // scenario-only: on a small map they may not have met by turn 5
					const int32_t args[1] = {(int32_t)t1t};
					gc_command cmd = Cmd(GC_CMD_DECLARE_WAR, humanList[0], args, 1);
					const int32_t wrc = gc_apply_command(inst, &cmd, &err);
					warHow = Format("turn %d: teams %d -> %d (met before: %d): DECLARE_WAR rc %d reason %d %s", turn, (int)t0t, (int)t1t, met ? 1 : 0, wrc, (int)err.reason, err.message);
					if (wrc != GC_OK)
					{
						GET_TEAM(t0t).declareWar(t1t);
						warHow += "; fallback CvTeam::declareWar";
					}
					warTurn = turn;
					printf("%s-war|%s\n", c.prefix.c_str(), warHow.c_str());
					continue;  // the bot's round and the next pump deliver it
				}
				if (hybrid && warTurn >= 0 && peaceTurn < 0 && turn >= warTurn + 6 && humanList.size() >= 2 && session->IsHumanWaiting((PlayerTypes)humanList[0]))
				{
					const TeamTypes t0t = GET_PLAYER((PlayerTypes)humanList[0]).getTeam(), t1t = GET_PLAYER((PlayerTypes)humanList[1]).getTeam();
					// N3.10: the white peace is the MAKE_PEACE command (the agreement of the two humans is the host's, GH-R11); a refusal falls back to CvTeam::makePeace so the
					// hybrid checks below still run, and fails the check peace_command.
					const int32_t peaceArgs[1] = {(int32_t)t1t};
					gc_command pcmd = Cmd(GC_CMD_MAKE_PEACE, humanList[0], peaceArgs, 1);
					const int32_t prc2 = gc_apply_command(inst, &pcmd, &err);
					peaceHow = Format("turn %d: teams %d -> %d: MAKE_PEACE rc %d reason %d %s", turn, (int)t0t, (int)t1t, prc2, (int)err.reason, err.message);
					if (prc2 != GC_OK)
					{
						GET_TEAM(t0t).makePeace(t1t);
						peaceHow += "; fallback CvTeam::makePeace";
					}
					peaceTurn = turn;
					printf("%s-peace|%s; at war afterwards: %d\n", c.prefix.c_str(), peaceHow.c_str(), GET_TEAM(t0t).isAtWar(t1t) ? 1 : 0);
					continue;
				}

				if (session->HumanPhase() && !humanList.empty())
				{
					int code = 0;
					for (size_t h = 0; h < humanList.size() && h < 8; ++h)
						if (GET_PLAYER((PlayerTypes)humanList[h]).isTurnActive()) code |= 1 << h;
					if (hybrid && warTurn >= 0 && turn > warTurn && humanList.size() >= 2)
					{
						// The turn mode changes from the turn after the declaration (CvGame::doTurn, setDynamicTurnsSimultMode); after the peace it changes back.
						const bool warring = peaceTurn < 0 || turn <= peaceTurn;
						if (warring)
						{
							++warringSteps;
							if ((code & 3) == 3) ++bothWarringActive;
							if (humanList.size() >= 3 && (code & 4) && (code & 3)) ++thirdWithOne;
							// N3.4: a command of a human waiting for its turn is NOT_YOUR_TURN.
							for (size_t h = 0; h < humanList.size(); ++h)
							{
								const PlayerTypes who = (PlayerTypes)humanList[h];
								if (GET_PLAYER(who).isTurnActive() || session->Engine().TurnCompleteSent(who)) continue;
								gc_command probe = Cmd(GC_CMD_END_TURN, (int)who, NULL, 0);
								const int32_t prcmd = gc_apply_command(inst, &probe, &err);
								++notYourTurn;
								if (prcmd == GC_ERR_REJECTED && err.reason == GC_REASON_NOT_YOUR_TURN) ++notYourTurnOk;
								else if (notYourTurnFirst.empty()) notYourTurnFirst = Format("turn %d player %d: rc %d reason %d \"%s\"", turn, (int)who, prcmd, (int)err.reason, err.message);
							}
						}
						else if (turn > peaceTurn && (code & 3) == 3) ++bothAfterPeace;
					}
					// Q_GAME v2 and Q_TURN (N3.6) against the engine: the same numbers the pump reports and the handshake holds.
						{
							++queryChecks;
							std::string bad;
							gc_buf bq = {0, 0, 0, 0}, bt = {0, 0, 0, 0};
							gcq_header ht;
							gcq_game qg;
							const int32_t qrc = gc_query(inst, GC_Q_GAME, NULL, 0, &bq, &err);
							if (qrc == GC_OK && bq.len >= sizeof(gcq_header) + sizeof(qg))
							{
								memcpy(&qg, bq.data + sizeof(gcq_header), sizeof(qg));
								int active = 0;
								for (size_t h = 0; h < humanList.size(); ++h) if (GET_PLAYER((PlayerTypes)humanList[h]).isTurnActive()) ++active;
								if (qg.game_type != 1) bad += Format(" game_type %d;", (int)qg.game_type);
								if (qg.turn_mode != (hybrid ? GC_TURN_MODE_HYBRID : GC_TURN_MODE_SIMULTANEOUS)) bad += Format(" turn_mode %d;", (int)qg.turn_mode);
								if (qg.humans != (int)humans) bad += Format(" humans %d;", (int)qg.humans);
								if (qg.humans_left != session->HumansLeft()) bad += Format(" humans_left %d vs %d;", (int)qg.humans_left, session->HumansLeft());
								if (qg.phase != GC_PHASE_HUMANS) bad += Format(" phase %d;", (int)qg.phase);
								if (qg.turn != turn) bad += Format(" turn %d vs %d;", (int)qg.turn, turn);
								if (qg.human_player != humanList[0]) bad += Format(" human_player %d;", (int)qg.human_player);
								if (qg.turn_len_s != 0 || qg.turn_base_s != 0) bad += Format(" timer %d/%d without a timer;", (int)qg.turn_len_s, (int)qg.turn_base_s);
								if (active == 0) bad += " nobody turn-active in the human phase;";
							}
							else bad += Format(" Q_GAME rc %d;", qrc);
							// Q_GAME for a player: the argument form.
							{
								gc_buf bp = {0, 0, 0, 0};
								const int32_t pa[1] = {(int32_t)humanList.back()};
								const int32_t prc2 = gc_query(inst, GC_Q_GAME, pa, 1, &bp, &err);
								if (prc2 != GC_OK || bp.len < sizeof(gcq_header) + sizeof(gcq_game)) bad += Format(" Q_GAME(player) rc %d;", prc2);
								else
								{
									gcq_game pg;
									memcpy(&pg, bp.data + sizeof(gcq_header), sizeof(pg));
									if (pg.human_player != humanList.back()) bad += Format(" Q_GAME(player) human_player %d;", (int)pg.human_player);
									const CvPlayer& lp = GET_PLAYER((PlayerTypes)humanList.back());
									const bool wantActive = lp.isTurnActive() && session->HumanPhase();
									if ((pg.active_player >= 0) != wantActive) bad += Format(" Q_GAME(player) active_player %d vs turn-active %d;", (int)pg.active_player, wantActive ? 1 : 0);
								}
								gc_buf_release(&bp);
							}
							if (gc_query(inst, GC_Q_TURN, NULL, 0, &bt, &err) == GC_OK && bt.len >= sizeof(ht))
							{
								memcpy(&ht, bt.data, sizeof(ht));
								int waitingFor = 0;
								if ((int)ht.count != (int)humans) { ++qturnCountBad; bad += Format(" Q_TURN count %d;", (int)ht.count); }
								for (uint32_t k = 0; k < ht.count && ht.record_size >= sizeof(gcq_turn); ++k)
								{
									gcq_turn tr;
									memcpy(&tr, bt.data + sizeof(ht) + (size_t)k * ht.record_size, sizeof(tr));
									const PlayerTypes who = (PlayerTypes)tr.player;
									if (tr.flags & GC_TURN_F_WAITING_FOR) ++waitingFor;
									if (((tr.flags & GC_TURN_F_TURN_ENDED) != 0) != session->Engine().TurnCompleteSent(who)) bad += Format(" p%d TURN_ENDED flag;", (int)tr.player);
									if (((tr.flags & GC_TURN_F_TURN_ACTIVE) != 0) != GET_PLAYER(who).isTurnActive()) bad += Format(" p%d TURN_ACTIVE flag;", (int)tr.player);
									if (((tr.flags & GC_TURN_F_SIMULTANEOUS) != 0) != GET_PLAYER(who).isSimultaneousTurns()) bad += Format(" p%d SIMULTANEOUS flag;", (int)tr.player);
									if (!(tr.flags & GC_TURN_F_HUMAN) || !(tr.flags & GC_TURN_F_ALIVE)) bad += Format(" p%d not HUMAN|ALIVE;", (int)tr.player);
									if (!hybrid && tr.turn_order != -1) bad += Format(" p%d turn_order %d in a simultaneous turn;", (int)tr.player, (int)tr.turn_order);
									if ((tr.flags & GC_TURN_F_CAN_END_TURN) && tr.blocker >= 0) bad += Format(" p%d CAN_END_TURN with blocker %d;", (int)tr.player, (int)tr.blocker);
								}
								if (waitingFor != session->HumansLeft()) bad += Format(" WAITING_FOR %d vs humans_left %d;", waitingFor, session->HumansLeft());
							}
							else bad += " Q_TURN failed;";
							gc_buf_release(&bq);
							gc_buf_release(&bt);
							if (!bad.empty())
							{
								++queryBad;
								if (queryFirstBad.empty()) queryFirstBad = Format("turn %d:%s", turn, bad.c_str());
							}
						}
						// END_TURN, then CANCEL_END_TURN of a waiting human: the end is taken back and the turn waits for the human again.
					if (!hybrid && !probeDone && turn >= startTurn + 3 && humanList.size() >= 1)
					{
						for (size_t h = 0; h < humanList.size() && !probeDone; ++h)
						{
							const PlayerTypes who = (PlayerTypes)humanList[h];
							if (!session->IsHumanWaiting(who) || session->CurrentBlocking(NULL, who) != NO_ENDTURN_BLOCKING_TYPE) continue;
							const int before = session->HumansLeft();
							gc_command endc = Cmd(GC_CMD_END_TURN, (int)who, NULL, 0);
							endRc = gc_apply_command(inst, &endc, &err);
							if (endRc != GC_OK) continue;
							const int ended = session->HumansLeft();
							gc_command cancel = Cmd(GC_CMD_CANCEL_END_TURN, (int)who, NULL, 0);
							cancelRc = gc_apply_command(inst, &cancel, &err);
							cancelReason = (int)err.reason;
							const int after = session->HumansLeft();
							cancelTurn = turn;
							cancelOk = (cancelRc == GC_OK && ended == before - 1 && after == before) ? 1 : 0;
							cancelNote = Format("player %d turn %d: humans left %d -> END_TURN rc %d -> %d -> CANCEL_END_TURN rc %d reason %d -> %d", (int)who, turn, before, endRc, ended, cancelRc, cancelReason, after);
							probeDone = true;
						}
					}
				}
				if (turn >= target) break;
			}
			endTurn = turn;
			turnEventsAtEnd = seen.events[GC_EVENT_TURN_STARTED];

			// N3.7 / N3.9: the per-player queries against the events and each other, at the human phase the run ended in (nothing pumped since: the events are complete).
			if (stopReason.empty())
			{
				uint64_t dStart = 0, dEnd = 0;
				const bool d0 = Digest2(inst, &dStart);
				ProbeNotesAndBars(inst, seen, humanList, seat, probes);
				ProbeTrees(inst, humanList, probes);
				ProbeDigests(inst, humanList, probes);
				const bool d1 = Digest2(inst, &dEnd);
				probes.queriesPure = d0 && d1 && dStart == dEnd;
				ProbePreview(inst, humanList, probes);
				ProbeMessages(inst, seen, humanList, probes);
				probes.ran = true;
			}

			// The game ends when every human is dead (decision 3.13); the server seat does not die.
			if (!hybrid && stopReason.empty())
			{
				for (size_t h = 0; h < humanList.size(); ++h)
				{
					CvPlayer& p = GET_PLAYER((PlayerTypes)humanList[h]);
					p.killUnits();
					p.killCities();
				}
				int over = 0;
				for (int k = 0; k < 400 && !over; ++k)
				{
					const int32_t prc = gc_pump(inst, 64, &pr, &err);
					if (prc != GC_OK && pr.status != GC_PUMP_GAME_OVER) { deadNote = Format("gc_pump: %d %s", prc, err.message); break; }
					if (pr.status == GC_PUMP_GAME_OVER) over = 1;
				}
				finalStatus = pr.status;
				finalGameOverEvents = seen.events[GC_EVENT_GAME_OVER];
				finalAlive = 0;
				for (size_t h = 0; h < humanList.size(); ++h)
					if (GET_PLAYER((PlayerTypes)humanList[h]).isAlive()) ++finalAlive;
				gc_pump_result again;
				memset(&again, 0, sizeof(again));
				const int32_t arc = gc_pump(inst, 8, &again, &err);
				deadNote += Format("%s%d human(s) alive after the kill; status %d after %s; pump again: rc %d status %d humans_left %d",
					deadNote.empty() ? "" : "; ", finalAlive, finalStatus, over ? "GAME_OVER" : "400 calls", arc, again.status, again.humans_left);
			}
		}
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("%s-event|%s\n", c.prefix.c_str(), seen.lines[i].c_str());

	c.add("turns", doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) (turn %d -> %d), %d gc_pump call(s)%s%s", doneTurns, turns, startTurn, endTurn, calls,
		stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0 && gameOver == 0 && pumpErrors == 0, "%d STUCK, %d GAME_OVER, %d gc_pump error(s)", stuck, gameOver, pumpErrors);
	c.add("turn_events", turnEventsAtEnd == doneTurns, "%d TURN_STARTED event(s) for %d turn(s)", turnEventsAtEnd, doneTurns);
	c.add("active_rule", activeRuleBroken == 0, "%d time(s) the active player was not the server seat %d after a gc_pump", activeRuleBroken, seat);
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);
	std::string botsJson;
	int botTurns = 0, botCommands = 0, botMinTurns = 1 << 30, botRejected = 0;
	for (size_t h = 0; h < humanList.size(); ++h)
	{
		const RtAutoplay* b = InstanceBot(inst, humanList[h]);
		if (!b) continue;
		const RtAutoplayStats& st = b->Stats();
		botTurns += st.turns;
		botCommands += st.commands;
		botRejected += st.rejected;
		botMinTurns = std::min(botMinTurns, st.turns);
		botsJson += std::string(botsJson.empty() ? "" : ",") + "{\"player\":" + Num(humanList[h]) + ",\"turns\":" + Num(st.turns) + ",\"forced\":" + Num(st.forced) + ",\"commands\":" + Num(st.commands) +
			",\"rejected\":" + Num(st.rejected) + ",\"founded\":" + Num(st.founded) + ",\"attacks\":" + Num(st.attacks) + ",\"retaken\":" + Num(st.retaken) + ",\"rounds\":" + Num(st.rounds) +
			",\"first_reject\":\"" + Escape(st.firstReject) + "\"}";
		printf("%s-human|player %d|bot turns %d forced %d commands %d refused %d founded %d attacks %d retaken %d|first refusal: %s\n", c.prefix.c_str(), humanList[h], st.turns, st.forced, st.commands,
			st.rejected, st.founded, st.attacks, st.retaken, st.firstReject.c_str());
	}
	if (botMinTurns == (1 << 30)) botMinTurns = 0;
	c.add("humans_played", !humanList.empty() && botMinTurns >= doneTurns && botCommands > 0, "%d human(s); the least active ended %d of %d turn(s) (%d in all), %d command(s) accepted, %d refused",
		(int)humanList.size(), botMinTurns, doneTurns, botTurns, botCommands, botRejected);
	c.add("humans_left", humansLeftWrong == 0 && humanPhaseTurns == doneTurns && humansLeftMax <= (int)humans,
		"%d wrong humans_left value(s) after a gc_pump; the turn waited for humans in %d of %d turn(s), most at once %d of %u", humansLeftWrong, humanPhaseTurns, doneTurns, humansLeftMax, humans);
	c.add("queries", queryChecks > 0 && queryBad == 0, "Q_GAME (v2 fields, by argument) and Q_TURN agreed with the engine in %d of %d human-phase step(s)%s%s", queryChecks - queryBad, queryChecks,
		queryFirstBad.empty() ? "" : "; first: ", queryFirstBad.c_str());
	{
		// PLAYER_TURN_STARTED for every human of every turn, PLAYER_TURN_ENDED when the end is sent, HUMAN_PHASE_STARTED once a turn.
		const int wantStarted = (int)humans * doneTurns;
		c.add("events", seen.events[GC_EVENT_PLAYER_TURN_STARTED] >= wantStarted && seen.endedSent >= wantStarted && seen.events[GC_EVENT_HUMAN_PHASE_STARTED] >= doneTurns &&
			seen.events[GC_EVENT_HUMAN_PHASE_STARTED] <= doneTurns + 1,
			"PLAYER_TURN_STARTED %d, PLAYER_TURN_ENDED %d sent / %d cancelled (at least %d each), HUMAN_PHASE_STARTED %d for %d turn(s)", seen.events[GC_EVENT_PLAYER_TURN_STARTED], seen.endedSent,
			seen.endedCancelled, wantStarted, seen.events[GC_EVENT_HUMAN_PHASE_STARTED], doneTurns);
	}
	if (probes.ran)
	{
		// N3.7: every human has his own notifications (events carry the player, the key is the pair), his own messages; the queries of a player agree with the events.
		int humansWithNotes = 0, notesAll = 0, notesNonHuman = 0, msgAll = 0, msgNonHuman = 0, popupAll = 0;
		std::string perHuman;
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			const bool isHuman = std::find(humanList.begin(), humanList.end(), i) != humanList.end();
			if (isHuman && seen.notesAdded[i] > 0) ++humansWithNotes;
			if (isHuman) perHuman += Format("%sp%d: notes +%d -%d messages %d popups %d", perHuman.empty() ? "" : "; ", i, seen.notesAdded[i], seen.notesRemoved[i], seen.messages[i], seen.popups[i]);
			notesAll += seen.notesAdded[i];
			msgAll += seen.messages[i];
			popupAll += seen.popups[i];
			if (!isHuman) { notesNonHuman += seen.notesAdded[i] + seen.notesRemoved[i]; msgNonHuman += seen.messages[i]; }
		}
		printf("%s-events-by-player|%s\n", c.prefix.c_str(), perHuman.c_str());
		c.add("notifications_by_player", humansWithNotes == (int)humanList.size() && notesNonHuman == 0 && seen.badShape == 0,
			"every human got NOTIFICATION_ADDED events with his player in the last argument (%d of %u humans, %d in all, %d for a non-human, %d malformed event(s) of kinds 3 / 4 / 7 / 11); %s", humansWithNotes,
			(unsigned)humanList.size(), notesAll, notesNonHuman, seen.badShape, perHuman.c_str());
		c.add("q_notifications_by_player", probes.humansAsked == (int)humanList.size() && probes.noteMismatch == 0 && probes.noteAiRecords == 0 && probes.noteSeatRecords == 0,
			"Q_NOTIFICATIONS(P) of each of %d human(s) lists exactly his live events (%d record(s) in all), an AI and the server seat none (%d / %d record(s))%s%s", probes.humansAsked, probes.noteTotal,
			probes.noteAiRecords, probes.noteSeatRecords, probes.noteFirst.empty() ? "" : "; first: ", probes.noteFirst.c_str());
		c.add("messages_by_player", msgNonHuman == 0 && seen.badShape == 0, "%d MESSAGE event(s) in all, every one for a seated human (%d for another player); %d POPUP_INFO event(s) with the player", msgAll, msgNonHuman, popupAll);
		c.add("top_bar_city_builds", probes.barFail == 0 && probes.otherBarFail == 0 && probes.buildsFail == 0 && probes.barAsked > 0,
			"Q_TOP_BAR of %d living major civ(s) (every human, an AI; the server seat is INVALID_ARG) and Q_CITY_BUILDS of %d own city(ies): %d / %d / %d failure(s)%s%s", probes.barAsked, probes.buildsAsked,
			probes.barFail, probes.otherBarFail, probes.buildsFail, probes.barFirst.empty() ? "" : "; first: ", probes.barFirst.c_str());
		c.add("message_to_the_human", probes.msgPlayer >= 0 && probes.msgAfter > probes.msgBefore && probes.msgOthers == 0, "%s", probes.msgNote.c_str());
		c.add("digest_by_team", probes.digestFail == 0, "%s", probes.digestNote.c_str());
		c.add("queries_pure", probes.queriesPure, "the simulation digest (viewer -2) is the same before and after the notification, top bar, city builds, path and tree queries");
		c.add("reachable_tree", probes.treeFail == 0 && probes.reachFail == 0 && probes.treeTrees > 0 && probes.treeSamples > 0,
			"%d unit(s) of the humans, %d tree(s) of horizon 1..3 with %d node(s): horizon 1 = Q_REACHABLE (%d mismatch), %d node(s) sampled against Q_PATH (turn, moves, flags, parent) and walked to the unit (%d failure(s), %d parent(s) outside the tree); mean ms %.1f / %.1f / %.1f; Q_PATH to a mountain: %d of %d UNREACHABLE%s%s",
			probes.treeUnits, probes.treeTrees, probes.treeNodes, probes.reachFail, probes.treeSamples, probes.treeFail, probes.treeOrphans, probes.treeCalls[1] ? probes.treeMs[1] / probes.treeCalls[1] : 0.0,
			probes.treeCalls[2] ? probes.treeMs[2] / probes.treeCalls[2] : 0.0, probes.treeCalls[3] ? probes.treeMs[3] / probes.treeCalls[3] : 0.0, probes.unreachOk, probes.unreachAsked,
			probes.treeFirst.empty() ? "" : "; first: ", probes.treeFirst.c_str());
		c.add("unreachable_path", probes.unreachAsked == 0 || probes.unreachOk == probes.unreachAsked, "Q_PATH to a plot without a path is one node with GC_REACH_F_UNREACHABLE (%d of %d)", probes.unreachOk, probes.unreachAsked);
		c.add("combat_preview", probes.previewAsked > 0 && probes.previewFail == 0 && probes.previewPure == probes.previewAsked, "%s", probes.previewNote.empty() ? "not run" : probes.previewNote.c_str());
	}
	if (hybrid)
	{
		c.add("war_event", seen.warEvents == 1 && warTurn >= 0 && humanList.size() >= 2 && seen.warArgs[0] == (int)GET_PLAYER((PlayerTypes)humanList[0]).getTeam() &&
			seen.warArgs[1] == (int)GET_PLAYER((PlayerTypes)humanList[1]).getTeam() && seen.warArgs[2] == 1,
			"%d HUMAN_WAR_DECLARED event(s); the first: from team %d to team %d, extends_timer %d", seen.warEvents, seen.warArgs[0], seen.warArgs[1], seen.warArgs[2]);
		c.add("war_declared", warTurn >= 0 && humanList.size() >= 2 && GET_TEAM(GET_PLAYER((PlayerTypes)humanList[0]).getTeam()).isAtWar(GET_PLAYER((PlayerTypes)humanList[1]).getTeam()) == (peaceTurn < 0),
			"%s", warHow.empty() ? "no war" : warHow.c_str());
		c.add("not_your_turn", notYourTurn > 0 && notYourTurnOk == notYourTurn, "%d command(s) of a human waiting for its turn refused: %d with GC_REASON_NOT_YOUR_TURN%s%s", notYourTurn, notYourTurnOk,
			notYourTurnFirst.empty() ? "" : "; first other: ", notYourTurnFirst.c_str());
		c.add("hybrid_order", warTurn >= 0 && warringSteps > 0 && bothWarringActive == 0, "from the turn after the war until the peace the two humans were turn-active together in %d of %d human-phase step(s)",
			bothWarringActive, warringSteps);
		if (humanList.size() >= 3)
			c.add("third_with_one", thirdWithOne > 0, "the peaceful third human was turn-active together with a warring one in %d step(s)", thirdWithOne);
		if (peaceTurn >= 0)
			c.add("peace_simultaneous", bothAfterPeace > 0, "peace at turn %d: the two humans were turn-active together again in %d step(s)", peaceTurn, bothAfterPeace);
	}
	else
	{
		c.add("cancel_end_turn", cancelOk == 1, "%s", cancelNote.empty() ? "no human with an open turn and no blocker was found" : cancelNote.c_str());
		c.add("all_humans_dead", finalStatus == GC_PUMP_GAME_OVER && finalAlive == 0 && finalGameOverEvents == 1, "%s", deadNote.empty() ? "not run" : deadNote.c_str());
	}

	std::string turnsJson;
	for (size_t i = 0; i < turnMs.size(); ++i) turnsJson += std::string(turnsJson.empty() ? "" : ",") + Format("%.1f", turnMs[i]);
	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"mode\":\"" + turnMode + "\",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) +
		",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"stuck\":" + Num(stuck) + ",\"game_over\":" + Num(gameOver) +
		",\"log_errors\":" + Num(seen.logErrors) + ",\"log_warnings\":" + Num(seen.logWarnings) + ",\"lua_errors\":" + Num(seen.luaErrors) + ",\"active_rule_broken\":" + Num(activeRuleBroken) +
		",\"humans_left_wrong\":" + Num(humansLeftWrong) + ",\"human_phase_turns\":" + Num(humanPhaseTurns) +
		",\"war\":{\"turn\":" + Num(warTurn) + ",\"how\":\"" + Escape(warHow) + "\",\"both_active_steps\":" + Num(bothWarringActive) + ",\"steps\":" + Num(warringSteps) + ",\"peace_turn\":" + Num(peaceTurn) +
		",\"both_after_peace\":" + Num(bothAfterPeace) + ",\"third_with_one\":" + Num(thirdWithOne) + "}" +
		",\"ms\":{\"turn_p50\":" + Format("%.1f", Percentile(turnMs, 0.5)) + ",\"turn_p95\":" + Format("%.1f", Percentile(turnMs, 0.95)) + ",\"turn_max\":" + Format("%.1f", Percentile(turnMs, 1.0)) +
		",\"total\":" + Format("%.1f", NowMs() - t0) + "},\"turn_ms\":[" + turnsJson + "],\"bots\":[" + botsJson + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("%s-summary|seed %u|humans %u|ai %u|turns %d/%u|stuck %d|log errors %d|lua errors %d|turn p50 %.1f ms max %.1f ms|%s\n", c.prefix.c_str(), seed, humans, ai, doneTurns, turns, stuck, seen.logErrors,
		seen.luaErrors, Percentile(turnMs, 0.5), Percentile(turnMs, 1.0), stopReason.c_str());
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
