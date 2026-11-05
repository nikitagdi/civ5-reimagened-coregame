// Scenario `humanturn` (task F1, milestone M4a; run.sh --scenario humanturn --turns N --seed S --ai A):
// RtSession::Load, NewGame({"seed":S,"ai_count":A,"auto_choices":{"research":0,"policy":0}}) (the human in slot 0; research and policy
// auto-choices off so the human's own research command is exercised), then for N turns:
//  1. Pump until WAITING_HUMAN; the turn must be the expected one and the status / blocker are recorded;
//  2. while there is a blocker: "end turn" (RtSession::EndTurn(false)) must be REJECTED_BLOCKED with that blocker; then the scenario
//     resolves the blocker as a UI would, with the human's commands through the loopback (gDLL->send*): research -> the first tech
//     CanResearch allows, free tech -> the first CanResearchForFree, production -> the first unit the city can train, units -> found a
//     city with a settler that can, otherwise skip (fortify / sleep when it cannot hold). On odd turns a UNITS blocker is left to FORCE_END_TURN
//     (EndTurn(true)). Any other blocker cannot be resolved here (it needs the auto-choices of F3-F5): the scenario stops and reports it;
//  3. "end turn" without a blocker must be ACCEPTED; a second one must be REJECTED_NOT_READY; the AI turn runs and the next WAITING_HUMAN
//     comes at turn + 1.
// G1b: on the first turn an Archer and a barbarian are spawned to check Q_RANGE_TARGETS and UNIT_RANGE_ATTACK, on the first turn with a
// city a barbarian next to it for CITY_RANGE_ATTACK (checks range_targets_unit / range_targets_city; the spawned units are removed again).
// Prints "humanturn-block|turn|blocker|target|action" lines (the observed blocker table) and "humanturn-turn|..." per turn.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "CvDllContext.h"
#include "CvGameCoreUtils.h"
#include "CvTechClasses.h"
#include "CvTypes.h"
#include "../abi/gc_command.h"
#include "../abi/gc_internal.h"
#include "../abi/gc_notify.h"
#include "../abi/gc_query_city.h"
#include "../abi/gc_query_range.h"
#include "rt_active.h"
#include "rt_diplo.h"
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
		printf("humanturn: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

std::string TargetText(const RtSession::BlockTarget& t)
{
	std::string s = RtSession::TargetKindName(t.kind);
	if (t.kind == RtSession::TARGET_UNIT || t.kind == RtSession::TARGET_CITY) s += Format(" %d/%d", t.owner, t.id);
	if (t.x >= 0) s += Format(" (%d,%d)", t.x, t.y);
	if (t.kind == RtSession::TARGET_UNIT)
	{
		const CvUnit* u = GET_PLAYER((PlayerTypes)t.owner).getUnit(t.id);
		if (u && u->getUnitInfo().GetType()) s += Format(" %s", u->getUnitInfo().GetType());
	}
	if (t.kind == RtSession::TARGET_CITY)
	{
		const CvCity* c = GET_PLAYER((PlayerTypes)t.owner).getCity(t.id);
		if (c) s += Format(" '%s'", c->getNameKey());
	}
	if (t.notification >= 0) s += Format(" [notification %d type 0x%08X]", t.notification, (unsigned)t.notificationType);
	return s;
}

// The human the scenario plays: the active player in a single-player game, the one SS_TAKEN slot of a network game (N3.4; then the active
// player is the server seat and the human's actions run under ActiveAs). NO_PLAYER before the game starts.
PlayerTypes g_human = NO_PLAYER;

// Pumps until the human is up (or the run cannot go on): never more than `budget` Pump calls. The result's blocker is the human's.
RtSession::PumpResult PumpToHuman(RtSession& s, int budget = 64)
{
	RtSession::PumpResult r;
	for (int i = 0; i < budget; ++i)
	{
		r = s.Pump(RtSession::kMaxSlicesPerTurn);
		if (r.status != RtSession::PUMP_AI_RUNNING) break;
	}
	if (g_human != NO_PLAYER) r.blocking = s.CurrentBlocking(&r.target, g_human);
	return r;
}

// Units the core's end-turn pass counts as awake (CvGame.cpp:9774): idle, nothing queued, can move, not automated.
int AwakeUnits(PlayerTypes who)
{
	CvPlayer& p = GET_PLAYER(who);
	int n = 0, loop;
	for (const CvUnit* u = p.firstUnit(&loop); u; u = p.nextUnit(&loop))
		if (u->GetLengthMissionQueue() == 0 && u->GetActivityType() == ACTIVITY_AWAKE && u->canMove() && !u->IsDoingPartialMove() && !u->IsAutomated()) ++n;
	return n;
}

// Q_CITY_BUILDS vs the command check vs the core: for every listed item "enabled" must equal what CheckCityOrder (CITY_SET_PRODUCTION)
// accepts, and every enabled item must really become the head of the order queue through CvCity::pushOrder (replace + append, as the
// command sends it). A disabled item must carry a reason. Counts into g_builds; the queue is emptied afterwards.
struct BuildsCheck { int checked, enabled, settlerSeen, settlerEnabled, mismatch, notPushed, noReason; std::string first; };
BuildsCheck g_builds = {0, 0, 0, 0, 0, 0, 0, ""};
bool g_buildsDone = false;

void CheckCityBuilds(CvCity* city)
{
	g_buildsDone = true;
	gc_buf b = {0, 0, 0, 0};
	std::string msg;
	const int32_t a[2] = {(int32_t)city->getOwner(), (int32_t)city->GetID()};
	ActiveAs as(city->getOwner());  // the query answers for the active player only (the gate goes with N3.7); gc_query does this itself
	if (RunCityQuery(GC_Q_CITY_BUILDS, a, 2, &b, msg) != GC_OK) { g_builds.first = "query failed: " + msg; ++g_builds.mismatch; return; }
	gcq_header h;
	memcpy(&h, b.data, sizeof(h));
	for (uint32_t i = 0; i < h.count; ++i)
	{
		gcq_city_build r;
		memcpy(&r, b.data + sizeof(h) + (size_t)i * h.record_size, sizeof(r));
		const bool en = (r.flags & GC_BUILD_F_ENABLED) != 0;
		const bool current = (r.flags & GC_BUILD_F_CURRENT) != 0;
		std::string why;
		const bool accepted = CheckCityOrder(city, r.order_type, r.item, why);
		++g_builds.checked;
		const bool settler = r.order_type == GC_ORDER_TRAIN && GC.getUnitInfo((UnitTypes)r.item) && GC.getUnitInfo((UnitTypes)r.item)->IsFound();
		if (settler) { ++g_builds.settlerSeen; if (en) ++g_builds.settlerEnabled; }
		if (!current && accepted != en)
		{
			++g_builds.mismatch;
			if (g_builds.first.empty()) g_builds.first = Format("order %d item %d: enabled=%d but the command check says %d (%s)", r.order_type, r.item, en ? 1 : 0, accepted ? 1 : 0, why.c_str());
		}
		if (!en && r.reason == GCQ_NO_STRING && r.order_type != GC_ORDER_MAINTAIN) ++g_builds.noReason;
		if (en && !current)
		{
			++g_builds.enabled;
			city->pushOrder((OrderTypes)r.order_type, r.item, -1, false, true, true);
			int ho = -1, hi = -1;
			if (!CityHeadOrder(city, &ho, &hi) || ho != r.order_type || hi != r.item || city->getOrderQueueLength() != 1)
			{
				++g_builds.notPushed;
				if (g_builds.first.empty()) g_builds.first = Format("order %d item %d is enabled but pushOrder left head %d/%d, queue length %d", r.order_type, r.item, ho, hi, (int)city->getOrderQueueLength());
			}
		}
	}
	gc_buf_release(&b);
	city->clearOrderQueue();
}

// The human's command that resolves the blocker (the loopback delivers it on the next Pump). Empty = cannot be resolved here.
std::string Resolve(RtSession& s, int blocking, const RtSession::BlockTarget& t)
{
	RtEngine& e = s.Engine();
	const PlayerTypes me = g_human != NO_PLAYER ? g_human : e.GetActivePlayer();
	ActiveAs as(me);  // the sends below go out as the active player
	CvPlayer& p = GET_PLAYER(me);
	switch (blocking)
	{
	case ENDTURN_BLOCKING_RESEARCH:
	case ENDTURN_BLOCKING_FREE_TECH:
	{
		const bool free = blocking == ENDTURN_BLOCKING_FREE_TECH;
		for (int i = 0; i < GC.getNumTechInfos(); ++i)
		{
			const TechTypes tech = (TechTypes)i;
			if (free ? p.GetPlayerTechs()->CanResearchForFree(tech) : p.GetPlayerTechs()->CanResearch(tech))
			{
				e.sendResearch(tech, free ? p.GetNumFreeTechs() : 0, NO_PLAYER, false);
				const CvTechEntry* info = GC.getTechInfo(tech);
				return Format("sendResearch(%s%s)", info ? info->GetType() : "?", free ? ", free" : "");
			}
		}
		return "";
	}
	case ENDTURN_BLOCKING_PRODUCTION:
	{
		CvCity* city = t.kind == RtSession::TARGET_CITY ? p.getCity(t.id) : NULL;
		if (!city) return "";
		if (!g_buildsDone) CheckCityBuilds(city);  // the freshly founded capital, once
		for (int i = 0; i < GC.getNumUnitInfos(); ++i)
		{
			const UnitTypes unit = (UnitTypes)i;
			if (city->canTrain(unit))
			{
				e.sendPushOrder(city->GetID(), ORDER_TRAIN, unit, false, false, false);
				return Format("sendPushOrder(city %d, ORDER_TRAIN, %s)", city->GetID(), GC.getUnitInfo(unit)->GetType());
			}
		}
		return "";
	}
	case ENDTURN_BLOCKING_UNITS:
	case ENDTURN_BLOCKING_STACKED_UNITS:
	{
		CvUnit* u = t.kind == RtSession::TARGET_UNIT ? p.getUnit(t.id) : NULL;
		if (!u) return "";
		MissionTypes m;
		const char* name;
		// Skip (not fortify) so the unit is ready again next turn and the UNITS blocker comes back for the FORCE_END_TURN turns.
		if (u->canFound(u->plot())) { m = CvTypes::getMISSION_FOUND(); name = "MISSION_FOUND"; }
		else if (u->canHold(u->plot())) { m = CvTypes::getMISSION_SKIP(); name = "MISSION_SKIP"; }
		else if (u->canFortify(u->plot())) { m = CvTypes::getMISSION_FORTIFY(); name = "MISSION_FORTIFY"; }
		else if (u->canSleep(u->plot())) { m = CvTypes::getMISSION_SLEEP(); name = "MISSION_SLEEP"; }
		else
		{
			// STACKED_UNITS (two units that cannot share the plot): walk this one to a neighbour plot it can enter.
			for (int d = 0; d < NUM_DIRECTION_TYPES; ++d)
			{
				CvPlot* to = plotDirection(u->getX(), u->getY(), (DirectionTypes)d);
				if (to && u->canMoveInto(*to))
				{
					e.sendPushMission(u->GetID(), CvTypes::getMISSION_MOVE_TO(), to->getX(), to->getY(), 0, false);
					return Format("sendPushMission(unit %d %s, MISSION_MOVE_TO (%d,%d))", u->GetID(), u->getUnitInfo().GetType(), to->getX(), to->getY());
				}
			}
			return "";
		}
		e.sendPushMission(u->GetID(), m, -1, -1, 0, false);
		return Format("sendPushMission(unit %d %s, %s)", u->GetID(), u->getUnitInfo().GetType(), name);
	}
	default:
		return "";
	}
}

std::string Num(long long v) { return Format("%lld", v); }

typedef std::map<int, std::pair<std::string, std::string> > NotifTexts;  // id -> (message, summary)

// Q_NOTIFICATIONS(player) through the ABI query writer; returns the record count (-1 on an error), fills got when not NULL.
int ReadNotifications(PlayerTypes who, NotifTexts* got)
{
	gc_buf b = {0, 0, 0, 0};
	std::string msg;
	const int32_t a[1] = {(int32_t)who};
	if (QueryNotifications(a, 1, &b, msg) != GC_OK) return -1;
	gcq_header h;
	memcpy(&h, b.data, sizeof(h));
	for (uint32_t i = 0; i < h.count; ++i)
	{
		gcq_notification n;
		memcpy(&n, b.data + sizeof(h) + (size_t)i * h.record_size, sizeof(n));
		const char* base = (const char*)b.data + h.strings_offset;
		if (got) (*got)[n.id] = std::make_pair(std::string(base + n.message), std::string(base + n.summary));
	}
	const int count = (int)h.count;
	gc_buf_release(&b);
	return count;
}

// ---- G1b: Q_RANGE_TARGETS against the core and the commands ----
// Unit test (first human turn): an Archer for the human next to one of its units and a barbarian Warrior next to the Archer.
// City test (first human turn with a city): a barbarian Warrior next to the city. For each shooter Q_RANGE_TARGETS must equal the
// core's canRangeStrikeAt over the whole map, hold the barbarian's plot, and be accepted by the range attack command; the command is
// then sent through ApplyHumanCommand, delivered by a pump, the barbarian must be hit, and the shooter has no targets left. The
// spawned units are removed afterwards so the rest of the run is the usual one.

typedef std::set<std::pair<int, int> > PlotSet;

// Q_RANGE_TARGETS(player, shooter) through the ABI query writer; false (msg) when the query fails or a record is malformed.
bool ReadRangeTargets(int player, int shooter, PlotSet& out, int& cityFlags, std::string& msg)
{
	out.clear();
	cityFlags = 0;
	gc_buf b = {0, 0, 0, 0};
	const int32_t a[2] = {(int32_t)player, (int32_t)shooter};
	if (QueryRangeTargets(a, 2, &b, msg) != GC_OK) return false;
	gcq_header h;
	memcpy(&h, b.data, sizeof(h));
	bool ok = memcmp(h.magic, "GCQ1", 4) == 0 && h.kind == GC_Q_RANGE_TARGETS && h.record_size == sizeof(gcq_range_target) &&
		b.len == sizeof(h) + (size_t)h.count * h.record_size && h.strings_size == 0;
	for (uint32_t i = 0; ok && i < h.count; ++i)
	{
		gcq_range_target r;
		memcpy(&r, b.data + sizeof(h) + (size_t)i * h.record_size, sizeof(r));
		if (!out.insert(std::make_pair((int)r.x, (int)r.y)).second || r.reserved != 0) ok = false;
		if (r.flags & GC_RANGE_TARGET_F_CITY) ++cityFlags;
	}
	gc_buf_release(&b);
	if (!ok) msg = "malformed Q_RANGE_TARGETS reply";
	return ok;
}

std::string PlotsText(const PlotSet& s)
{
	std::string t;
	for (PlotSet::const_iterator it = s.begin(); it != s.end(); ++it) t += Format("%s(%d,%d)", t.empty() ? "" : " ", it->first, it->second);
	return t.empty() ? "none" : t;
}

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

int32_t SendShot(RtSession& s, int kind, int player, int shooter, int x, int y, std::string& msg)
{
	const int32_t args[3] = {(int32_t)shooter, (int32_t)x, (int32_t)y};
	gc_command cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.kind = (uint16_t)kind;
	cmd.player = (uint8_t)player;
	cmd.argc = 3;
	cmd.args = args;
	return ApplyHumanCommand(s, cmd, msg);
}

struct RangeCheck
{
	bool done;
	bool ok;
	std::string detail;
	RangeCheck() : done(false), ok(false) {}
};
RangeCheck g_rangeUnit, g_rangeCity;

// Shared part: the reply equals `reference`, holds the barbarian, the command takes every target and refuses a plot outside.
void CompareTargets(RangeCheck& rc, RtSession& s, int cmdKind, int player, int shooterArg, int cmdShooter, const PlotSet& reference, const CvUnit* barb, int originX, int originY)
{
	PlotSet got;
	int cityFlags = 0;
	std::string msg;
	if (!ReadRangeTargets(player, shooterArg, got, cityFlags, msg)) { rc.detail = "query failed: " + msg; return; }
	const std::pair<int, int> bp(barb->getX(), barb->getY());
	int refused = 0;
	std::string why;
	for (PlotSet::const_iterator it = got.begin(); it != got.end(); ++it)
	{
		// The command check is the same as the query's; ApplyHumanCommand would queue it, so only the check is asked here.
		if (cmdKind == GC_CMD_UNIT_RANGE_ATTACK)
		{
			CvUnit* u = GET_PLAYER((PlayerTypes)player).getUnit(cmdShooter);
			if (!u || !CheckUnitCommand(u, GC_CMD_UNIT_RANGE_ATTACK, true, it->first, it->second, why)) ++refused;
		}
		else
		{
			CvCity* c = GET_PLAYER((PlayerTypes)player).getCity(cmdShooter);
			if (!c || !CheckCityRangeAttack(c, it->first, it->second, why)) ++refused;
		}
	}
	// A plot that is not a target (the shooter's own plot) is refused by the command with a reason.
	std::string ownWhy;
	const int32_t own = SendShot(s, cmdKind, player, cmdShooter, originX, originY, ownWhy);
	rc.ok = got == reference && got.count(bp) == 1 && refused == 0 && cityFlags == 0 && own == GC_ERR_REJECTED && !ownWhy.empty();
	rc.detail = Format("targets %s; core canRangeStrikeAt %s; barbarian at (%d,%d) %s; refused by the command check %d; own plot -> %d (%s)", PlotsText(got).c_str(),
		PlotsText(reference).c_str(), bp.first, bp.second, got.count(bp) ? "listed" : "MISSING", refused, own, ownWhy.c_str());
}

// Fires at the barbarian through the command, pumps, and checks the hit and that no target is left.
void ShootAndCheck(RangeCheck& rc, RtSession& s, int cmdKind, int player, int shooterArg, int cmdShooter, int barbId, int x, int y)
{
	if (!rc.ok) return;
	CvPlayer& barbs = GET_PLAYER(BARBARIAN_PLAYER);
	std::string msg;
	const int32_t sent = SendShot(s, cmdKind, player, cmdShooter, x, y, msg);
	if (sent != GC_OK) { rc.ok = false; rc.detail += Format("; the shot was refused (%d): %s", sent, msg.c_str()); return; }
	// G1: the events up to here are set aside so the shot's own GC_EVENT_MESSAGE events can be told apart; all are put back in order afterwards.
	RtEventQueue& q = s.Events();
	std::vector<RtEvent> before;
	RtEvent ev;
	while (q.Pop(ev)) before.push_back(ev);
	const RtSession::PumpResult r = PumpToHuman(s);
	std::vector<RtEvent> after;
	while (q.Pop(ev)) after.push_back(ev);
	int msgs = 0, msgsOk = 0;
	std::string firstMsg;
	for (size_t i = 0; i < after.size(); ++i)
	{
		const RtEvent& e = after[i];
		if (e.kind != GC_EVENT_MESSAGE) continue;
		++msgs;
		const bool plotOk = (e.args[1] == -1 && e.args[2] == -1) || (e.args[1] >= 0 && e.args[2] >= 0 && e.args[1] < GC.getMap().getGridWidth() && e.args[2] < GC.getMap().getGridHeight());
		if (e.argc == 6 && e.args[0] == player && plotOk && e.args[4] >= 0 && e.args[4] <= 2 && e.hasText && !e.text.empty())
		{
			++msgsOk;
			if (firstMsg.empty()) firstMsg = e.text;
		}
	}
	for (size_t i = 0; i < before.size(); ++i) q.Push(before[i]);
	for (size_t i = 0; i < after.size(); ++i) q.Push(after[i]);
	const CvUnit* barb = barbs.getUnit(barbId);
	const bool hit = !barb || barb->isDelayedDeath() || barb->getDamage() > 0;
	PlotSet left;
	int cityFlags = 0;
	const bool read = ReadRangeTargets(player, shooterArg, left, cityFlags, msg);
	rc.ok = r.status == RtSession::PUMP_WAITING_HUMAN && hit && read && left.empty() && msgsOk > 0 && msgsOk == msgs;
	rc.detail += Format("; shot at (%d,%d): barbarian %s; targets after the shot: %s; %d GC_EVENT_MESSAGE for the human (%d well-formed), first: \"%s\"", x, y,
		!barb ? "killed" : Format("damage %d", barb->getDamage()).c_str(), read ? PlotsText(left).c_str() : msg.c_str(), msgs, msgsOk, firstMsg.c_str());
}

void KillUnit(PlayerTypes owner, int id)
{
	CvUnit* u = GET_PLAYER(owner).getUnit(id);
	if (u && !u->isDelayedDeath()) u->kill(false);
}

PlotSet CoreUnitTargets(const CvUnit* u)
{
	PlotSet s;
	CvMap& map = GC.getMap();
	for (int y = 0; y < map.getGridHeight(); ++y)
		for (int x = 0; x < map.getGridWidth(); ++x)
			if (u->canRangeStrikeAt(x, y)) s.insert(std::make_pair(x, y));
	return s;
}

PlotSet CoreCityTargets(const CvCity* c)
{
	PlotSet s;
	CvMap& map = GC.getMap();
	for (int y = 0; y < map.getGridHeight(); ++y)
		for (int x = 0; x < map.getGridWidth(); ++x)
			if (c->canRangeStrikeAt(x, y)) s.insert(std::make_pair(x, y));
	return s;
}

void RangeUnitTest(RtSession& s, PlayerTypes me)
{
	RangeCheck& rc = g_rangeUnit;
	rc.done = true;
	const UnitTypes archerType = (UnitTypes)GC.getInfoTypeForString("UNIT_ARCHER", true);
	const UnitTypes warriorType = (UnitTypes)GC.getInfoTypeForString("UNIT_WARRIOR", true);
	if (archerType == NO_UNIT || warriorType == NO_UNIT) { rc.detail = "no UNIT_ARCHER / UNIT_WARRIOR in the database"; return; }
	CvPlayer& p = GET_PLAYER(me);
	CvPlot* at = NULL;
	CvPlot* barbAt = NULL;
	int loop;
	for (CvUnit* u = p.firstUnit(&loop); u && !barbAt; u = p.nextUnit(&loop))
	{
		at = FreeLandNextTo(u->getX(), u->getY(), -1, -1);
		if (at) barbAt = FreeLandNextTo(at->getX(), at->getY(), u->getX(), u->getY());
	}
	if (!at || !barbAt) { rc.detail = "no free land for an Archer and a barbarian next to the human's units"; return; }
	CvUnit* archer = p.initUnit(archerType, at->getX(), at->getY(), NO_UNITAI, NO_DIRECTION, false, false);
	CvUnit* barb = GET_PLAYER(BARBARIAN_PLAYER).initUnit(warriorType, barbAt->getX(), barbAt->getY(), NO_UNITAI, NO_DIRECTION, false, false);
	if (!archer || !barb) { rc.detail = "initUnit failed"; return; }
	const int archerId = archer->GetID(), barbId = barb->GetID();
	const int bx = barb->getX(), by = barb->getY();
	CompareTargets(rc, s, GC_CMD_UNIT_RANGE_ATTACK, (int)me, archerId, archerId, CoreUnitTargets(archer), barb, archer->getX(), archer->getY());
	rc.detail = Format("Archer %d at (%d,%d): ", archerId, archer->getX(), archer->getY()) + rc.detail;
	ShootAndCheck(rc, s, GC_CMD_UNIT_RANGE_ATTACK, (int)me, archerId, archerId, barbId, bx, by);
	KillUnit(BARBARIAN_PLAYER, barbId);
	KillUnit(me, archerId);
}

void RangeCityTest(RtSession& s, PlayerTypes me, CvCity* city)
{
	RangeCheck& rc = g_rangeCity;
	rc.done = true;
	const UnitTypes warriorType = (UnitTypes)GC.getInfoTypeForString("UNIT_WARRIOR", true);
	CvPlot* barbAt = FreeLandNextTo(city->getX(), city->getY(), -1, -1);
	if (warriorType == NO_UNIT || !barbAt) { rc.detail = "no free land next to the city for a barbarian"; return; }
	// Nothing in range yet: no targets.
	PlotSet before;
	int cityFlags = 0;
	std::string msg;
	const bool readBefore = ReadRangeTargets((int)me, -city->GetID(), before, cityFlags, msg);
	CvUnit* barb = GET_PLAYER(BARBARIAN_PLAYER).initUnit(warriorType, barbAt->getX(), barbAt->getY(), NO_UNITAI, NO_DIRECTION, false, false);
	if (!barb) { rc.detail = "initUnit failed"; return; }
	const int barbId = barb->GetID();
	const int bx = barb->getX(), by = barb->getY();
	CompareTargets(rc, s, GC_CMD_CITY_RANGE_ATTACK, (int)me, -city->GetID(), city->GetID(), CoreCityTargets(city), barb, city->getX(), city->getY());
	rc.ok = rc.ok && readBefore && before.empty();
	rc.detail = Format("city %d at (%d,%d), targets before the barbarian: %s; ", city->GetID(), city->getX(), city->getY(), readBefore ? PlotsText(before).c_str() : msg.c_str()) + rc.detail;
	ShootAndCheck(rc, s, GC_CMD_CITY_RANGE_ATTACK, (int)me, -city->GetID(), city->GetID(), barbId, bx, by);
	if (rc.ok && !city->isMadeAttack()) { rc.ok = false; rc.detail += "; the city has not made its attack"; }
	KillUnit(BARBARIAN_PLAYER, barbId);
}

// ---- N3.4: the refusal codes of HumanMayAct and CANCEL_END_TURN / END_TURN_TIMEOUT ----

// ApplyHumanCommand for `player` with a command of no arguments; the result code and the reason.
int32_t Send0(RtSession& s, int kind, int player, int32_t* reason, std::string& msg)
{
	gc_command cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.kind = (uint16_t)kind;
	cmd.player = (uint8_t)player;
	cmd.argc = 0;
	cmd.args = NULL;
	return ApplyHumanCommand(s, cmd, msg, reason);
}

struct ReasonStats
{
	int notHuman, notHumanOk, aiRunning, aiRunningOk, turnEnded, turnEndedOk, cancelOk, cancelAgainOk, timeoutOk, activeLeaks;
	std::string detail;
	ReasonStats() : notHuman(0), notHumanOk(0), aiRunning(0), aiRunningOk(0), turnEnded(0), turnEndedOk(0), cancelOk(0), cancelAgainOk(0), timeoutOk(0), activeLeaks(0) {}
};
ReasonStats g_reasons;

void Expect(ReasonStats& st, int& seen, int& ok, const char* what, int32_t rc, int32_t reason, int32_t want, const std::string& msg)
{
	++seen;
	if (rc == GC_ERR_REJECTED && reason == want) ++ok;
	else if (st.detail.size() < 400) st.detail += Format("%s: rc %d reason %d (want %d) \"%s\"; ", what, rc, reason, want, msg.c_str());
}

// A command for a player that is no human of the game (an AI civ, the server seat, a slot beyond the majors) is NOT_HUMAN.
void ProbeNotHuman(RtSession& s)
{
	std::vector<int> who;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if (GET_PLAYER((PlayerTypes)i).isAlive() && !GET_PLAYER((PlayerTypes)i).isHuman()) { who.push_back(i); break; }
	if (ServerSeat() != NO_PLAYER) who.push_back((int)ServerSeat());
	who.push_back(200);
	for (size_t i = 0; i < who.size(); ++i)
	{
		int32_t reason = -1;
		std::string msg;
		const int32_t rc = Send0(s, GC_CMD_END_TURN, who[i], &reason, msg);
		Expect(g_reasons, g_reasons.notHuman, g_reasons.notHumanOk, Format("END_TURN player %d", who[i]).c_str(), rc, reason, GC_REASON_NOT_HUMAN, msg);
	}
	if (ServerSeat() != NO_PLAYER && CvPreGame::activePlayer() != ServerSeat()) ++g_reasons.activeLeaks;
}

// The human has just ended the turn (nothing pumped since): a command is TURN_ENDED. In a network game the end of turn is taken back
// (CANCEL_END_TURN; the next pump delivers NetTurnUnready and the human is up again in the same turn), taken back again (idempotent), and the
// turn is ended by END_TURN_TIMEOUT instead of END_TURN. Returns false (why) when the game does not go on as expected.
bool ProbeEndedTurn(RtSession& s, PlayerTypes me, bool net, std::string& why)
{
	int32_t reason = -1;
	std::string msg;
	int32_t rc = Send0(s, GC_CMD_END_TURN, (int)me, &reason, msg);
	Expect(g_reasons, g_reasons.turnEnded, g_reasons.turnEndedOk, "END_TURN after the end of turn", rc, reason, GC_REASON_TURN_ENDED, msg);
	if (!net) return true;
	const int turn = GC.getGame().getGameTurn();
	rc = Send0(s, GC_CMD_CANCEL_END_TURN, (int)me, &reason, msg);
	if (rc != GC_OK || s.Engine().TurnCompleteSent(me)) { why = Format("CANCEL_END_TURN rc %d reason %d \"%s\", still ended %d", rc, reason, msg.c_str(), s.Engine().TurnCompleteSent(me) ? 1 : 0); return false; }
	++g_reasons.cancelOk;
	const RtSession::PumpResult r = PumpToHuman(s);
	if (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != turn)
	{
		why = Format("after CANCEL_END_TURN the human was not up again in turn %d (%s at turn %d: %s)", turn, RtSession::PumpStatusName(r.status), r.turn, r.reason.c_str());
		return false;
	}
	if (s.Engine().HasReceivedTurnComplete(me)) { why = "after CANCEL_END_TURN the loopback still holds the end of turn"; return false; }
	rc = Send0(s, GC_CMD_CANCEL_END_TURN, (int)me, &reason, msg);
	if (rc == GC_OK) ++g_reasons.cancelAgainOk;
	else { why = Format("a second CANCEL_END_TURN (nothing to take back) rc %d \"%s\"", rc, msg.c_str()); return false; }
	// A command is accepted again (not TURN_ENDED / NOT_YOUR_TURN): END_TURN while a blocker may hold it is BLOCKED or accepted.
	rc = Send0(s, GC_CMD_END_TURN_TIMEOUT, (int)me, &reason, msg);
	if (rc != GC_OK || !s.Engine().TurnCompleteSent(me)) { why = Format("END_TURN_TIMEOUT rc %d reason %d \"%s\"", rc, reason, msg.c_str()); return false; }
	++g_reasons.timeoutOk;
	return true;
}

// ---- N3.4: the same through the C ABI of a network game (the guard of gc_abi.cpp, gc_error.reason, the queries answered as the human) ----

int g_leakLogs = 0;
void LeakCountSink(void* user, int level, const char* msg)
{
	if (level == LOG_ERROR && strstr(msg, "active-player rule broken") != NULL) { ++g_leakLogs; return; }
	ForwardLog(user, level, msg);
}

// At WAITING_HUMAN of a network game (the active player is the server seat): after each C ABI call the active player is the server seat again.
void AbiProbe(gc_instance* inst, PlayerTypes me, const gc_host* host, Check& c)
{
	const PlayerTypes seat = ServerSeat();
	int calls = 0, leaks = 0;
	std::string detail;
	gc_error err;
	memset(&err, 0, sizeof(err));
	gc_buf b = {0, 0, 0, 0};
	int unitId = -1;
	int loop;
	CvUnit* u0 = GET_PLAYER(me).firstUnit(&loop);
	if (u0) unitId = u0->GetID();
	int ai = -1;
	for (int i = 0; i < MAX_MAJOR_CIVS && ai < 0; ++i)
		if (GET_PLAYER((PlayerTypes)i).isAlive() && !GET_PLAYER((PlayerTypes)i).isHuman()) ai = i;
#define AFTER_CALL() do { ++calls; if (CvPreGame::activePlayer() != seat) ++leaks; } while (0)
	// Queries about the human, answered as that human: Q_TOP_BAR only answers the active player, so this works only under ActiveAs.
	const int32_t qa[1] = {(int32_t)me};
	int32_t rc = gc_query(inst, GC_Q_TOP_BAR, qa, 1, &b, &err);
	AFTER_CALL();
	const bool topBar = rc == GC_OK && b.len > sizeof(gcq_header);
	if (!topBar) detail += Format("Q_TOP_BAR(human) rc %d %s; ", (int)rc, err.message);
	gc_buf_release(&b);
	const int32_t qu[2] = {(int32_t)me, (int32_t)unitId};
	rc = gc_query(inst, GC_Q_UNIT_ACTIONS, qu, 2, &b, &err);
	AFTER_CALL();
	const bool actions = rc == GC_OK;
	if (!actions) detail += Format("Q_UNIT_ACTIONS(human, unit) rc %d %s; ", (int)rc, err.message);
	gc_buf_release(&b);
	// Commands: the reason codes of gc_error.reason through the ABI.
	gc_command cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.kind = GC_CMD_END_TURN;
	cmd.player = (uint8_t)(ai < 0 ? 200 : ai);
	rc = gc_apply_command(inst, &cmd, &err);
	AFTER_CALL();
	const bool notHuman = rc == GC_ERR_REJECTED && err.reason == GC_REASON_NOT_HUMAN;
	if (!notHuman) detail += Format("END_TURN as an AI: rc %d reason %d; ", (int)rc, (int)err.reason);
	const int32_t skipArgs[1] = {999999};
	cmd.kind = GC_CMD_UNIT_SKIP;
	cmd.player = (uint8_t)me;
	cmd.argc = 1;
	cmd.args = skipArgs;
	rc = gc_apply_command(inst, &cmd, &err);
	AFTER_CALL();
	const bool badTarget = rc == GC_ERR_REJECTED && err.reason == GC_REASON_BAD_TARGET;
	if (!badTarget) detail += Format("UNIT_SKIP of a unit that does not exist: rc %d reason %d; ", (int)rc, (int)err.reason);
	// (No gc_pump here: it would drain the event queue the scenario counts below.)
	c.add("abi_as_player", topBar && actions && notHuman && badTarget && leaks == 0, "%d C ABI call(s) at WAITING_HUMAN: Q_TOP_BAR and Q_UNIT_ACTIONS of the human answered, a command as an AI player -> NOT_HUMAN, "
		"a missing unit -> BAD_TARGET, the active player left on the server seat %d after each call (%d leak(s))%s%s", calls, (int)seat, leaks, detail.empty() ? "" : "; ", detail.c_str());
	// The guard itself: a call that leaves the wrong active player logs an error and the field is repaired.
	g_leakLogs = 0;
	SetLogSink(LeakCountSink, const_cast<gc_host*>(host));
	{
		ActiveRuleGuard guard("test", true);
		CvPreGame::setActivePlayer(me);
	}
	const bool repaired = CvPreGame::activePlayer() == seat;
	{
		ActiveRuleGuard guard("test", true);  // nothing broken: silent
	}
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	c.add("active_rule_guard", g_leakLogs == 1 && repaired, "a leaked active player: %d error log(s) (want 1), repaired %d; a clean exit logs nothing", g_leakLogs, repaired ? 1 : 0);
#undef AFTER_CALL
}

}  // namespace

extern "C" int rt_selftest_humanturn(const gc_host* host, unsigned seed, unsigned turns, unsigned ai, const char* game_type, char* report, size_t report_len)
{
	const bool net = game_type && strcmp(game_type, "network") == 0;
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;

	// Left alive on purpose, as in scenario `load`. Network: the instance goes through the C ABI (gc_create / gc_new_game), so the scenario's probes can
	// call gc_apply_command / gc_query / gc_pump with the active-player rule guard of gc_abi.cpp in place; the session is the instance's.
	gc_instance* inst = NULL;
	gc_error abiErr;
	memset(&abiErr, 0, sizeof(abiErr));
	RtSession* session = NULL;
	RtSession::LoadStatus ls;
	bool loaded;
	if (net)
	{
		inst = gc_create(host, &abiErr);
		session = InstanceSession(inst);
		loaded = inst != NULL && session != NULL && session->Loaded();
		c.add("load", loaded, "gc_create: %d %s", (int)abiErr.code, abiErr.message);
	}
	else
	{
		session = new RtSession();
		loaded = session->Load(*host, ls);
		c.add("load", loaded, "boot reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());
	}

	int doneTurns = 0, startTurn = -1, endTurn = -1, luaErrors = 0, rejectedBlocked = 0, rejectedMismatch = 0, forced = 0, resolved = 0;
	int waitingOk = 0, acceptedOk = 0, secondRejected = 0, stuck = 0;
	int forceSkipped = 0, diploTotal = 0, diploDeclined = 0, diploPending = 0, diploRequests = 0;
	std::string diploText, diploJson;
	// F7: the event queue the ABI drains (rt_events.h) and Q_NOTIFICATIONS over the same turns.
	int evCount[16] = {0}, evNotifTextOk = 0, evLive = 0, evDiploTextOk = 0, evTurnArgsOk = 0, evLuaErrors = 0, ssErrorsTotal = 0;
	int qCount = -1, qMatch = 0, qCountAfter = -1, dismissedId = -1, removedEvents = 0, qTextOk = 0, regNowCount = 0;
	bool evChecked = false;
	std::vector<std::string> evLines;
	std::string stopReason, table, turnsJson;
	std::map<std::string, std::string> blockerKinds;  // blocker -> example target
	if (loaded)
	{
		RtSession::NewGameStatus st;
		// Network: the same one human in slot 0 (Rome) and `ai` random AI, the server seat as active player.
		std::string players = "{\"kind\":\"human\",\"civ\":\"CIVILIZATION_ROME\",\"nick\":\"bot0\"}";
		for (unsigned i = 0; i < ai; ++i) players += ",{\"kind\":\"ai\",\"civ\":\"random\"}";
		const std::string setup = net ? Format("{\"game_type\":\"network\",\"seed\":%u,\"players\":[%s],\"auto_choices\":{\"research\":0,\"policy\":0}}", seed, players.c_str())
			: Format("{\"game_type\":\"single\",\"seed\":%u,\"ai_count\":%u,\"auto_choices\":{\"research\":0,\"policy\":0}}", seed, ai);
		bool started;
		if (net)
		{
			started = gc_new_game(inst, setup.c_str(), &abiErr) == GC_OK;
			st.turn = GC.getGame().getGameTurn();
			c.add("new_game", started, "%s: gc_new_game %d %s", setup.c_str(), (int)abiErr.code, abiErr.message);
		}
		else
		{
			started = session->NewGame(*host, setup, st);
			c.add("new_game", started, "%s reached step %s%s%s", setup.c_str(), RtSession::NewGameStepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());
		}
		RtScriptSystem* ss = session->Scripts();
		if (started && ss)
		{
			const int luaBefore = ss->GetErrorCount();
			PlayerTypes me = session->Engine().GetActivePlayer();
			if (net)
			{
				for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
					if (IsSeatedHuman(i)) { me = (PlayerTypes)i; break; }
				c.add("active_is_server_seat", ServerSeat() != NO_PLAYER && session->Engine().GetActivePlayer() == ServerSeat() && me != session->Engine().GetActivePlayer(),
					"network game: active player %d = server seat %d, the human is player %d", (int)session->Engine().GetActivePlayer(), (int)ServerSeat(), (int)me);
			}
			g_human = me;
			c.add("human_setup", me != NO_PLAYER && GET_PLAYER(me).isHuman() && GET_PLAYER(me).isAlive(), "human player %d (active %d) human=%d alive=%d units=%d",
				(int)me, (int)session->Engine().GetActivePlayer(), me != NO_PLAYER && GET_PLAYER(me).isHuman() ? 1 : 0, me != NO_PLAYER && GET_PLAYER(me).isAlive() ? 1 : 0,
				me != NO_PLAYER ? GET_PLAYER(me).getNumUnits() : 0);
			startTurn = st.turn;
			for (unsigned k = 0; k < turns && stopReason.empty(); ++k)
			{
				const int expectTurn = startTurn + (int)k;
				RtSession::PumpResult r = PumpToHuman(*session);
				if (r.status == RtSession::PUMP_STUCK) ++stuck;
				if (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != expectTurn)
				{
					stopReason = Format("expected WAITING_HUMAN at turn %d, got %s at turn %d: %s", expectTurn, RtSession::PumpStatusName(r.status), r.turn, r.reason.c_str());
					break;
				}
				++waitingOk;
				// G1b: range targets of a unit (first turn) and of the first city (first turn that has one); the probes pump, so the
				// turn state is read again afterwards.
				{
					int loopCity;
					CvCity* city = GET_PLAYER(me).firstCity(&loopCity);
					const bool unitNow = !g_rangeUnit.done;
					const bool cityNow = !g_rangeCity.done && city != NULL;
					if (unitNow) RangeUnitTest(*session, me);
					if (cityNow) RangeCityTest(*session, me, city);
					if (unitNow || cityNow)
					{
						r = PumpToHuman(*session);
						if (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != expectTurn)
						{
							stopReason = Format("turn %d: after the range attack probes expected WAITING_HUMAN, got %s at turn %d: %s", expectTurn, RtSession::PumpStatusName(r.status), r.turn,
								r.reason.c_str());
							break;
						}
					}
				}
				if (net && k == 0) AbiProbe(inst, me, host, c);
				const int firstBlocking = r.blocking;
				std::string actions;
				int steps = 0;
				bool turnEnded = false;
				while (stopReason.empty() && !turnEnded)
				{
					if (++steps > 64)
					{
						stopReason = Format("turn %d: the blocker %s did not go away after 64 commands", r.turn, RtSession::BlockingName(r.blocking));
						break;
					}
					const std::string targetText = TargetText(r.target);
					if (r.blocking != NO_ENDTURN_BLOCKING_TYPE)
					{
						// "End turn" while blocked must be rejected with this blocker.
						RtSession::EndTurnResult e = session->EndTurn(false, me);
						const bool match = e.status == RtSession::END_TURN_REJECTED_BLOCKED && e.blocking == r.blocking && e.target.kind == r.target.kind &&
							e.target.id == r.target.id && e.target.x == r.target.x && e.target.y == r.target.y;
						if (e.status == RtSession::END_TURN_REJECTED_BLOCKED) ++rejectedBlocked;
						if (!match) ++rejectedMismatch;
						std::string action;
						const bool unitsBlocker = r.blocking == ENDTURN_BLOCKING_UNITS || r.blocking == ENDTURN_BLOCKING_STACKED_UNITS;
						// CvGame::UpdateMoves (CvGame.cpp:9774) lets only the first awake unit consume the FORCE_END_TURN flag; the second one takes the end turn
						// back (sendTurnUnready), every time. With more than one awake unit the force cannot work in the core, so the scenario skips them instead.
						const bool tryForce = unitsBlocker && (k & 1) && AwakeUnits(me) <= 1;
						if (unitsBlocker && (k & 1) && !tryForce) ++forceSkipped;
						if (tryForce)
						{
							RtSession::EndTurnResult f = session->EndTurn(true, me);
							action = Format("EndTurn(force) -> %s%s%s", RtSession::EndTurnStatusName(f.status), f.reason.empty() ? "" : ": ", f.reason.c_str());
							if (f.status == RtSession::END_TURN_ACCEPTED) { ++forced; turnEnded = true; }
						}
						if (!turnEnded)
						{
							std::string cmd = Resolve(*session, r.blocking, r.target);
							if (cmd.empty())
							{
								action = action.empty() ? "unresolved (needs an auto-choice, tasks F3-F5)" : action + "; unresolved";
								stopReason = Format("turn %d: blocker %s (target %s) cannot be resolved by this scenario", r.turn, RtSession::BlockingName(r.blocking),
									targetText.c_str());
							}
							else
							{
								action = action.empty() ? cmd : action + "; " + cmd;
								++resolved;
							}
						}
						const std::string line = Format("%d|%s|%s|EndTurn -> %s (%s)|%s", r.turn, RtSession::BlockingName(r.blocking), targetText.c_str(),
							RtSession::EndTurnStatusName(e.status), match ? "matches" : "MISMATCH", action.c_str());
						printf("humanturn-block|%s\n", line.c_str());
						table += std::string(table.empty() ? "" : ",") + "\"" + Escape(line) + "\"";
						if (!blockerKinds.count(RtSession::BlockingName(r.blocking))) blockerKinds[RtSession::BlockingName(r.blocking)] = targetText;
						actions += (actions.empty() ? "" : "; ") + action;
						if (!stopReason.empty() || turnEnded) break;
						// Deliver the command and wait for the human again.
						r = PumpToHuman(*session);
						if (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != expectTurn)
						{
							stopReason = Format("turn %d: after a command expected WAITING_HUMAN, got %s at turn %d: %s", expectTurn, RtSession::PumpStatusName(r.status), r.turn,
								r.reason.c_str());
							if (r.status == RtSession::PUMP_STUCK) ++stuck;
						}
						continue;
					}
					// No blocker: "end turn" must be accepted.
					RtSession::EndTurnResult e = session->EndTurn(false, me);
					printf("humanturn-block|%d|NONE|-|EndTurn -> %s%s%s|-\n", r.turn, RtSession::EndTurnStatusName(e.status), e.reason.empty() ? "" : ": ", e.reason.c_str());
					if (e.status != RtSession::END_TURN_ACCEPTED)
					{
						stopReason = Format("turn %d: EndTurn without a blocker was %s: %s", r.turn, RtSession::EndTurnStatusName(e.status), e.reason.c_str());
						break;
					}
					++acceptedOk;
					turnEnded = true;
				}
				if (!turnEnded) break;
				// A second "end turn" in the same turn is refused (turn complete already sent).
				RtSession::EndTurnResult again = session->EndTurn(false, me);
				const bool againRejected = again.status == RtSession::END_TURN_REJECTED_NOT_READY;
				// N3.4: the refusal codes, once, right after the first end of turn (nothing pumped since); network: take the end of turn back and end it by timeout.
				if (k == 0)
				{
					ProbeNotHuman(*session);
					std::string why;
					if (!ProbeEndedTurn(*session, me, net, why)) { stopReason = Format("turn %d: %s", expectTurn, why.c_str()); break; }
				}
				// The AI turn runs; the next WAITING_HUMAN must be at turn + 1 (checked at the top of the next iteration).
				RtSession::PumpResult n;
				for (int i = 0; i < 64; ++i)
				{
					n = session->Pump(RtSession::kMaxSlicesPerTurn, true);
					if (n.status != RtSession::PUMP_AI_RUNNING || n.turnsAdvanced > 0) break;
				}
				if (n.turnsAdvanced != 1 || n.turn != expectTurn + 1)
				{
					if (n.status == RtSession::PUMP_STUCK) ++stuck;
					stopReason = Format("turn %d: after the accepted end turn the game did not reach turn %d (%s at turn %d: %s)", expectTurn, expectTurn + 1,
						RtSession::PumpStatusName(n.status), n.turn, n.reason.c_str());
					break;
				}
				// The new turn has just started and the AI civs have not moved: a command of the human is AI_RUNNING (the host may queue it).
				if (n.status == RtSession::PUMP_AI_RUNNING)
				{
					int32_t reason = -1;
					std::string msg;
					const int32_t rc = Send0(*session, GC_CMD_END_TURN, (int)me, &reason, msg);
					Expect(g_reasons, g_reasons.aiRunning, g_reasons.aiRunningOk, "END_TURN while the AI moves", rc, reason, GC_REASON_AI_RUNNING, msg);
				}
				++doneTurns;
				if (againRejected) ++secondRejected;
				endTurn = n.turn;
				printf("humanturn-turn|%d -> %d|first blocker %s|%s|lua errors %d\n", expectTurn, n.turn, RtSession::BlockingName(firstBlocking), actions.empty() ? "-" : actions.c_str(),
					ss->GetErrorCount() - luaBefore);
				turnsJson += std::string(turnsJson.empty() ? "" : ",") + "{\"turn\":" + Num(expectTurn) + ",\"first_blocker\":\"" + RtSession::BlockingName(firstBlocking) +
					"\",\"actions\":\"" + Escape(actions) + "\"}";
			}
			if (stopReason.empty() && doneTurns == (int)turns)
			{
				// The human is up again at the final turn.
				RtSession::PumpResult r = PumpToHuman(*session);
				if (r.status != RtSession::PUMP_WAITING_HUMAN || r.turn != startTurn + (int)turns)
					stopReason = Format("expected WAITING_HUMAN at turn %d after the last end turn, got %s at turn %d", startTurn + (int)turns, RtSession::PumpStatusName(r.status), r.turn);
				else
					++waitingOk;
			}
			luaErrors = ss->GetErrorCount() - luaBefore;
			c.add("city_builds_agree", g_buildsDone && g_builds.checked > 0 && g_builds.mismatch == 0 && g_builds.notPushed == 0 && g_builds.noReason == 0 && g_builds.enabled > 0 &&
				g_builds.settlerEnabled == 0 && g_builds.settlerSeen > 0,
				"Q_CITY_BUILDS of the first city: %d items, %d enabled all pushed, settler listed %d enabled %d (pop 1: must be disabled), mismatches %d, not pushed %d, disabled without reason %d%s%s",
				g_builds.checked, g_builds.enabled, g_builds.settlerSeen, g_builds.settlerEnabled, g_builds.mismatch, g_builds.notPushed, g_builds.noReason, g_builds.first.empty() ? "" : "; ", g_builds.first.c_str());
			c.add("range_targets_unit", g_rangeUnit.done && g_rangeUnit.ok, "Q_RANGE_TARGETS of an Archer next to a barbarian: %s", g_rangeUnit.done ? g_rangeUnit.detail.c_str() : "not run");
			c.add("range_targets_city", g_rangeCity.done && g_rangeCity.ok, "Q_RANGE_TARGETS of the first city with a barbarian next to it: %s", g_rangeCity.done ? g_rangeCity.detail.c_str() : "not run (no city)");
			ssErrorsTotal = ss->GetErrorCount();
			// F6 (SIMP-003): AI diplomacy towards the human, declined by the host after every Update.
			RtDiplo& dp = session->Diplo();
			diploTotal = dp.Total();
			diploDeclined = dp.Declined();
			diploPending = dp.Pending();
			for (std::map<int, int>::const_iterator it = dp.CountsByState().begin(); it != dp.CountsByState().end(); ++it)
				diploText += Format("%s%s=%d", diploText.empty() ? "" : ", ", RtDiplo::StateName(it->first), it->second);
			for (int a = 0; a < NUM_RTDIPLO_ANSWERS; ++a)
				diploJson += Format("%s\"%s\":%d", diploJson.empty() ? "" : ",", RtDiplo::AnswerName(a), dp.AnswerCount((RtDiploAnswer)a));
			{
				RtEventQueue& q = session->Events();
				std::set<int> live;
				NotifTexts texts;
				RtEvent e;
				int prevTurn = startTurn;
				while (q.Pop(e))
				{
					if (e.kind >= 0 && e.kind < 16) ++evCount[e.kind];
					if (e.kind == GC_EVENT_TURN_STARTED && e.argc == 1 && e.args[0] == prevTurn + 1) { ++evTurnArgsOk; prevTurn = e.args[0]; }
					if (e.kind == GC_EVENT_NOTIFICATION_ADDED && e.argc >= 6 && (e.argc < 7 || e.args[6] == (int)me) && e.hasText && e.hasText2 && !e.text.empty() && !e.text2.empty())
					{
						++evNotifTextOk;
						live.insert(e.args[0]);
						texts[e.args[0]] = std::make_pair(e.text, e.text2);
						evLines.push_back(Format("notification #%d type %08x at (%d,%d) data %d %d: \"%s\" / \"%s\"", e.args[0], (unsigned)e.args[1], e.args[2], e.args[3], e.args[4], e.args[5],
							e.text2.c_str(), e.text.c_str()));
					}
					if (e.kind == GC_EVENT_NOTIFICATION_REMOVED && e.argc >= 1 && (e.argc < 2 || e.args[1] == (int)me)) { live.erase(e.args[0]); ++removedEvents; }
					if (e.kind == GC_EVENT_DIPLO_AUTO_DECLINED && e.argc >= 2 && e.hasText && e.hasText2 && !e.text2.empty())
					{
						++evDiploTextOk;
						evLines.push_back(Format("diplo AI %d state %d: \"%s\" | %s", e.args[0], e.args[1], e.text.c_str(), e.text2.c_str()));
					}
					if (e.kind == GC_EVENT_LUA_ERROR) { ++evLuaErrors; evLines.push_back("lua error: " + e.text); }
					if (e.kind == GC_EVENT_POPUP_INFO) evLines.push_back(Format("popup type %d data %d %d %d: \"%s\"", e.args[0], e.args[1], e.args[2], e.args[3], e.hasText ? e.text.c_str() : "(no text)"));
					if (e.kind == GC_EVENT_WAR_STATE && e.argc == 3 && e.args[1] != 63) evLines.push_back(Format("war state teams %d %d at war %d", e.args[0], e.args[1], e.args[2]));
				}
				evLive = (int)live.size();
				evChecked = true;
				// Q_NOTIFICATIONS of the human against the event view and the registry.
				NotifTexts got;
				qCount = ReadNotifications(me, &got);
				std::vector<RtNotification> regNow;
				session->UI().NotificationsOf(me, regNow);
				regNowCount = (int)regNow.size();
				qMatch = (qCount == evLive && qCount == regNowCount) ? 1 : 0;
				for (NotifTexts::const_iterator it = got.begin(); it != got.end(); ++it)
				{
					NotifTexts::const_iterator t = texts.find(it->first);
					if (t != texts.end() && t->second == it->second) ++qTextOk;
				}
				// RemoveNotification: dismiss one live notification the way the UI does and see event, registry and query follow.
				CvNotifications* nf = me != NO_PLAYER ? GET_PLAYER(me).GetNotifications() : NULL;
				for (size_t ri = 0; nf && ri < regNow.size(); ++ri)
					if (nf->MayUserDismiss(regNow[ri].id)) { dismissedId = regNow[ri].id; break; }
				if (dismissedId >= 0)
				{
					nf->Dismiss(dismissedId, true);
					session->UI().SyncNetworkNotifications();  // network: the REMOVED event comes from the comparison (the registry hook is off)
					int removedNow = 0;
					while (q.Pop(e))
						if (e.kind == GC_EVENT_NOTIFICATION_REMOVED && e.argc >= 1 && e.args[0] == dismissedId) ++removedNow;
					qCountAfter = ReadNotifications(me, NULL);
					if (!(removedNow == 1 && qCountAfter == qCount - 1 && !session->UI().FindNotificationOf(me, dismissedId, NULL))) dismissedId = -2;
				}
			}
			const std::vector<RtDiploEvent>& evs = dp.Events();
			for (size_t i = 0; i < evs.size(); ++i)
			{
				if (evs[i].request) ++diploRequests;
				printf("humanturn-diplo|%s\n", RtDiplo::Describe(evs[i]).c_str());
			}
		}
	}

	std::string kinds;
	for (std::map<std::string, std::string>::const_iterator it = blockerKinds.begin(); it != blockerKinds.end(); ++it)
		kinds += std::string(kinds.empty() ? "" : ", ") + it->first + " -> " + it->second;
	c.add("waiting_human", loaded && waitingOk == doneTurns + 1 && stopReason.empty(), "WAITING_HUMAN reached %d time(s) at the expected turn", waitingOk);
	c.add("rejected_while_blocked", rejectedBlocked > 0 && rejectedMismatch == 0, "%d end turn(s) while blocked rejected with the reported blocker, %d mismatch(es)", rejectedBlocked,
		rejectedMismatch);
	c.add("blockers_resolved", resolved > 0, "%d blocker(s) resolved by human commands, %d turn(s) ended with FORCE_END_TURN; blockers seen: %s", resolved, forced,
		kinds.empty() ? "none" : kinds.c_str());
	c.add("force_end_turn", turns < 2 || forced > 0, "%d turn(s) ended with FORCE_END_TURN over a UNITS blocker, %d odd turn(s) resolved by skipping because several units were awake (the core cancels FORCE_END_TURN then)", forced, forceSkipped);
	c.add("end_turn_accepted",acceptedOk + forced == doneTurns && doneTurns > 0, "%d end turn(s) accepted without a blocker, %d forced", acceptedOk, forced);
	c.add("second_end_turn_rejected", secondRejected == doneTurns, "%d/%d repeated end turn(s) rejected as not ready", secondRejected, doneTurns);
	c.add("turns", loaded && doneTurns == (int)turns && stopReason.empty(), "%d/%u human turn(s) done (turn %d -> %d)%s%s", doneTurns, turns, startTurn, endTurn,
		stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0, "%d stuck turn(s)", stuck);
	c.add("diplo_answered", diploPending == 0, "%d AI diplomacy message(s) to the human, %d with a refusal (%d were requests: m_bRequestActive); answers {%s}; by state: %s; %d unanswered at the end",
		diploTotal, diploDeclined, diploRequests, diploJson.c_str(), diploText.empty() ? "none" : diploText.c_str(), diploPending);
	if (evChecked)
	{
		for (size_t i = 0; i < evLines.size() && i < 60; ++i) printf("humanturn-event|%s\n", evLines[i].c_str());
		printf("humanturn-events|turn_started %d|player_turn_started %d|notification_added %d|notification_removed %d|popup %d|diplo %d|war_state %d|lua_error %d|live %d\n", evCount[GC_EVENT_TURN_STARTED],
			evCount[GC_EVENT_PLAYER_TURN_STARTED], evCount[GC_EVENT_NOTIFICATION_ADDED], evCount[GC_EVENT_NOTIFICATION_REMOVED], evCount[GC_EVENT_POPUP_INFO], evCount[GC_EVENT_DIPLO_AUTO_DECLINED],
			evCount[GC_EVENT_WAR_STATE], evCount[GC_EVENT_LUA_ERROR], evLive);
		c.add("event_turns", evCount[GC_EVENT_TURN_STARTED] == endTurn - startTurn && evTurnArgsOk == evCount[GC_EVENT_TURN_STARTED], "%d TURN_STARTED event(s) for turns %d -> %d, %d with consecutive turn args", evCount[GC_EVENT_TURN_STARTED], startTurn, endTurn, evTurnArgsOk);
		// Single game: the registry; network game (N3.7): the core's CvNotifications of the human compared by SyncNetworkNotifications.
		{
			c.add("event_player_turn", evCount[GC_EVENT_PLAYER_TURN_STARTED] >= doneTurns, "%d PLAYER_TURN_STARTED event(s) for %d human turn(s)", evCount[GC_EVENT_PLAYER_TURN_STARTED], doneTurns);
			c.add("event_notifications", evCount[GC_EVENT_NOTIFICATION_ADDED] > 0 && evNotifTextOk == evCount[GC_EVENT_NOTIFICATION_ADDED] && evCount[GC_EVENT_NOTIFICATION_REMOVED] > 0,
				"%d NOTIFICATION_ADDED (%d with message and summary text), %d NOTIFICATION_REMOVED, %d still live", evCount[GC_EVENT_NOTIFICATION_ADDED], evNotifTextOk, evCount[GC_EVENT_NOTIFICATION_REMOVED], evLive);
			c.add("q_notifications", qMatch == 1 && qTextOk == qCount, "Q_NOTIFICATIONS(human) lists %d record(s) = the live events = the registry / the core's list (%d), %d with the event text", qCount, regNowCount, qTextOk);
			c.add("remove_notification", dismissedId >= 0, "dismissing notification %d: one REMOVED event, registry entry gone, Q_NOTIFICATIONS %d -> %d (-1 = no dismissable notification live, -2 = mismatch)", dismissedId, qCount, qCountAfter);
		}
		c.add("event_diplo", evCount[GC_EVENT_DIPLO_AUTO_DECLINED] == diploDeclined && evDiploTextOk == diploDeclined, "%d DIPLO_AUTO_DECLINED event(s) = %d declined message(s), %d with text2", evCount[GC_EVENT_DIPLO_AUTO_DECLINED], diploDeclined, evDiploTextOk);
		c.add("event_lua_errors", evCount[GC_EVENT_LUA_ERROR] == evLuaErrors && evLuaErrors == ssErrorsTotal, "%d LUA_ERROR event(s) = %d Lua error(s) counted by the script system", evLuaErrors, ssErrorsTotal);
	}
	c.add("lua_errors_turns", loaded && luaErrors == 0, "%d Lua error(s) during the turns", luaErrors);
	// N3.4: HumanMayAct names why a command is refused (gc_error.reason): not a human, the turn ended, the AI moves; network: CANCEL_END_TURN and END_TURN_TIMEOUT.
	{
		const ReasonStats& rs = g_reasons;
		c.add("reason_codes", loaded && doneTurns > 0 && rs.notHuman >= 2 && rs.notHumanOk == rs.notHuman && rs.turnEnded == 1 && rs.turnEndedOk == 1 && rs.aiRunning >= 1 && rs.aiRunningOk == rs.aiRunning &&
			rs.activeLeaks == 0,
			"NOT_HUMAN %d/%d (an AI civ%s, a slot beyond the majors), TURN_ENDED %d/%d, AI_RUNNING %d/%d%s%s", rs.notHumanOk, rs.notHuman, net ? ", the server seat" : "", rs.turnEndedOk, rs.turnEnded,
			rs.aiRunningOk, rs.aiRunning, rs.detail.empty() ? "" : "; ", rs.detail.c_str());
		if (net)
			c.add("cancel_end_turn", loaded && doneTurns > 0 && rs.cancelOk == 1 && rs.cancelAgainOk == 1 && rs.timeoutOk == 1,
				"CANCEL_END_TURN took the end of turn back (%d), again with nothing to take back (%d, idempotent), END_TURN_TIMEOUT ended the turn (%d)", rs.cancelOk, rs.cancelAgainOk, rs.timeoutOk);
	}
	if (net) c.add("active_rule", CvPreGame::activePlayer() == ServerSeat(), "after the run the active player is %d, the server seat is %d", (int)CvPreGame::activePlayer(), (int)ServerSeat());

	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"seed\":" + Num(seed) + ",\"ai_count\":" + Num(ai) + ",\"turns_requested\":" + Num(turns) +
		",\"turns_done\":" + Num(doneTurns) + ",\"start_turn\":" + Num(startTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"forced\":" + Num(forced) +
		",\"diplo_total\":" + Num(diploTotal) + ",\"diplo_declined\":" + Num(diploDeclined) + ",\"diplo_requests\":" + Num(diploRequests) +
		",\"diplo_answers\":{" + diploJson + "},\"stop_reason\":\"" + Escape(stopReason) + "\",\"blocker_table\":[" + table + "],\"turns\":[" + turnsJson + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("humanturn-summary|seed %u|ai %u|turns %d|forced %d|rejected %d|diplo %d (declined %d)|blockers %s\n", seed, ai, doneTurns, forced, rejectedBlocked, diploTotal, diploDeclined, kinds.c_str());
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
