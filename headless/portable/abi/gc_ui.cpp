// B4 M2 WO-1: the UI bridge of a replica (gamecore_abi.h "UI bridge"): the DLL's Lua libraries in the UI's Lua state, the event sink, the
// interface state of the exe (selection, interface mode, city screen) and the graphical setup after a load. C++03.
#include "CvGameCoreDLLPCH.h"

#include "gamecore_abi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../runtime/rt_active.h"
#include "../runtime/rt_lua.h"
#include "../runtime/rt_lua_gameinfo.h"
#include "../runtime/rt_session.h"
#include "../runtime/rt_ui_replica.h"
#include "../runtime/rt_ui_deal.h"
#include "gc_internal.h"
#include "lekmod_portable/host_log.h"

#include "CvDllCity.h"
#include "CvDllUnit.h"
#include "CvDllContext.h"

using lekmod_portable::RtSession;
using lekmod_portable::RtUiReplica;

namespace {

int32_t Fail(gc_error* err, int32_t code, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int32_t Fail(gc_error* err, int32_t code, const char* fmt, ...)
{
	if (err)
	{
		err->code = code;
		err->reason = GC_REASON_NONE;
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(err->message, sizeof(err->message), fmt, ap);
		va_end(ap);
		err->message[sizeof(err->message) - 1] = '\0';
	}
	return code;
}

int32_t Ok(gc_error* err)
{
	if (err)
	{
		err->code = GC_OK;
		err->message[0] = '\0';
		err->reason = GC_REASON_NONE;
	}
	return GC_OK;
}

// A running game, for the entries that need one.
int32_t NeedGame(gc_instance* inst, gc_error* err)
{
	if (!inst) return Fail(err, GC_ERR_INVALID_ARG, "instance is NULL");
	RtSession* s = lekmod_portable::InstanceSession(inst);
	if (!s || !s->InGame()) return Fail(err, GC_ERR_NOT_STARTED, "no game is running");
	return GC_OK;
}

int32_t NeedReplica(gc_instance* inst, gc_error* err, const char* entry)
{
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	if (lekmod_portable::InstanceRole(inst) != GC_ROLE_REPLICA) return Fail(err, GC_ERR_REJECTED, "%s: only a replica (GC_ROLE_REPLICA) has a UI", entry);
	return GC_OK;
}

#define GC_UI_GUARD_BEGIN try {
#define GC_UI_GUARD_END(err)                                                                   \
	}                                                                                          \
	catch (const std::exception& e)                                                            \
	{                                                                                          \
		lekmod_portable::Log(GC_LOG_ERROR, "gamecore: exception: %s", e.what());               \
		return Fail(err, GC_ERR_INTERNAL, "exception: %s", e.what());                          \
	}                                                                                          \
	catch (...)                                                                                \
	{                                                                                          \
		return Fail(err, GC_ERR_INTERNAL, "unknown exception");                                \
	}

// ---- gc_lua_attach_ui ------------------------------------------------------------------------------------------------------------------

struct AttachArgs
{
	gc_instance* inst;
	std::string error;
};

// Runs under lua_cpcall: a Lua error raised by a registration comes back as an error, not as a longjmp through the caller.
int AttachBody(lua_State* L)
{
	AttachArgs* a = static_cast<AttachArgs*>(lua_touserdata(L, 1));
	RtSession* s = lekmod_portable::InstanceSession(a->inst);
	ICvScriptSystemUtility1* libs = s->Context() ? s->Context()->GetScriptSystemUtility() : NULL;
	if (!libs) return luaL_error(L, "the DLL has no script system utility");
	libs->RegisterScriptLibraries(L);
	if (!lekmod_portable::RegisterGameInfo(L, s->Db().Connection())) return luaL_error(L, "GameInfo and DB.Query: the game database has no connection");
	lekmod_portable::RegisterExeGameDefines(L, s->Db().Connection());
	lekmod_portable::RegisterExePreGame(L);
	lekmod_portable::RegisterUiDealLib(L, libs, &s->Engine());  // B4 M4 WO-D: GcUi* (the trade screen's UI.* functions)
	return 0;
}

// ---- gc_lua_invalidate_ui ----------------------------------------------------------------------------------------------------------------

// The Lua types whose instances are pointers to game objects that a load frees and creates again. Player, Team and TeamTech are static arrays and stay valid.
// ("Deal" is not here since B4 M4 WO-D: the trade screen keeps the scratch deal (rt_ui_deal.cpp, owned by the process, not by the game) in a Lua
// variable for good; the deals of the game are only read in passing by the overviews.)
const char* const kRecreatedTypes[] = {"Unit", "City", "Plot", "Area", "League"};

int InvalidateBody(lua_State* L)
{
	const bool plots = *static_cast<const bool*>(lua_touserdata(L, 1));
	for (size_t i = 0; i < sizeof(kRecreatedTypes) / sizeof(kRecreatedTypes[0]); ++i)
	{
		if (!plots && strcmp(kRecreatedTypes[i], "Plot") == 0) continue;
		lua_getglobal(L, kRecreatedTypes[i]);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			continue;
		}
		lua_pushstring(L, "__instances");
		lua_rawget(L, -2);
		if (lua_istable(L, -1))
		{
			// Every cached instance loses its pointer (a script may keep one in a variable); the cache is emptied in the same pass (assigning nil to the
			// field being visited is allowed while traversing).
			lua_pushnil(L);
			while (lua_next(L, -2) != 0)
			{
				if (lua_istable(L, -1))
				{
					lua_pushstring(L, "__instance");
					lua_pushnil(L);
					lua_rawset(L, -3);
				}
				lua_pop(L, 1);          // the value
				lua_pushvalue(L, -1);   // the key
				lua_pushnil(L);
				lua_rawset(L, -4);      // __instances[key] = nil
			}
		}
		lua_pop(L, 2);  // __instances (or its absence), the type table
	}
	return 0;
}

// ---- gc_set_ui_sink / the ops ----------------------------------------------------------------------------------------------------------

struct Pair
{
	int32_t owner, id;
};

bool Need(uint32_t have, uint32_t want, gc_error* err, int32_t op)
{
	if (have >= want) return true;
	Fail(err, GC_ERR_INVALID_ARG, "gc_ui_op %d needs %u input value(s), got %u", (int)op, (unsigned)want, (unsigned)have);
	return false;
}

struct Out
{
	int32_t* p;
	uint32_t cap;
	uint32_t n;
	Out(int32_t* out, uint32_t* n_out) : p(out), cap(out && n_out ? *n_out : 0), n(0) {}
	void Put(int32_t v)
	{
		if (n < cap) p[n] = v;
		++n;
	}
};

}  // namespace

extern "C" int32_t gc_lua_attach_ui(gc_instance* inst, struct lua_State* L, gc_error* err)
{
	if (!inst || !L) return Fail(err, GC_ERR_INVALID_ARG, "instance and L are required");
	GC_UI_GUARD_BEGIN
	const int32_t need = NeedGame(inst, err);
	if (need != GC_OK) return need;
	const int top = lua_gettop(L);
	AttachArgs a;
	a.inst = inst;
	const int rc = lua_cpcall(L, AttachBody, &a);
	std::string msg;
	if (rc != 0)
	{
		const char* m = lua_tostring(L, -1);
		msg = m ? m : "?";
	}
	lua_settop(L, top);
	if (rc != 0) return Fail(err, GC_ERR_INTERNAL, "gc_lua_attach_ui: %s", msg.c_str());
	return Ok(err);
	GC_UI_GUARD_END(err)
}

extern "C" int32_t gc_lua_invalidate_ui(gc_instance* inst, struct lua_State* L, gc_error* err)
{
	if (!inst) return Fail(err, GC_ERR_INVALID_ARG, "instance is NULL");
	if (!L) return Ok(err);
	GC_UI_GUARD_BEGIN
	const int top = lua_gettop(L);
	bool plots = true;  // see the notes in gamecore_abi.h on which types a resync recreates
	const int rc = lua_cpcall(L, InvalidateBody, &plots);
	std::string msg;
	if (rc != 0)
	{
		const char* m = lua_tostring(L, -1);
		msg = m ? m : "?";
	}
	lua_settop(L, top);
	if (rc != 0) return Fail(err, GC_ERR_INTERNAL, "gc_lua_invalidate_ui: %s", msg.c_str());
	return Ok(err);
	GC_UI_GUARD_END(err)
}

extern "C" int32_t gc_set_ui_sink(gc_instance* inst, const gc_ui_sink* sink, gc_error* err)
{
	if (!inst) return Fail(err, GC_ERR_INVALID_ARG, "instance is NULL");
	GC_UI_GUARD_BEGIN
	RtSession* s = lekmod_portable::InstanceSession(inst);
	s->UiReplica().SetSink(sink);
	return Ok(err);
	GC_UI_GUARD_END(err)
}

extern "C" int32_t gc_ui_subscribe_game_event(gc_instance* inst, const char* name, gc_error* err)
{
	if (!inst || !name || !name[0]) return Fail(err, GC_ERR_INVALID_ARG, "instance and name are required");
	GC_UI_GUARD_BEGIN
	lekmod_portable::InstanceSession(inst)->UiReplica().Subscribe(name);
	return Ok(err);
	GC_UI_GUARD_END(err)
}

extern "C" int32_t gc_ui_setup_graphical(gc_instance* inst, gc_error* err)
{
	if (!inst) return Fail(err, GC_ERR_INVALID_ARG, "instance is NULL");
	GC_UI_GUARD_BEGIN
	const int32_t need = NeedReplica(inst, err, "gc_ui_setup_graphical");
	if (need != GC_OK) return need;
	RtSession* s = lekmod_portable::InstanceSession(inst);
	RtUiReplica& rep = s->UiReplica();
	// At rest the active player is the seat: the fog and visibility the setup works out are the UI's own.
	rep.Revalidate();
	rep.SetRebuild(true);
	try
	{
		GC.SetGraphicsInitialized(true);
		GC.getMap().setupGraphical();
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			CvPlayer& p = GET_PLAYER((PlayerTypes)i);
			if (p.isAlive() || p.isEverAlive()) p.setupGraphical();
		}
		rep.SyncFog();
		s->UI().SyncReplicaNotifications();
	}
	catch (...)
	{
		rep.SetRebuild(false);
		throw;
	}
	rep.SetRebuild(false);
	return Ok(err);
	GC_UI_GUARD_END(err)
}

extern "C" int32_t gc_ui_op(gc_instance* inst, int32_t op, const int32_t* in, uint32_t n_in, int32_t* out, uint32_t* n_out, gc_error* err)
{
	if (!inst || (n_in > 0 && !in)) return Fail(err, GC_ERR_INVALID_ARG, "instance and well-formed input are required");
	GC_UI_GUARD_BEGIN
	const int32_t need = NeedReplica(inst, err, "gc_ui_op");
	if (need != GC_OK) return need;
	if (op <= 0 || op >= GC_UI_OP_COUNT) return Fail(err, GC_ERR_INVALID_ARG, "gc_ui_op: unknown op %d", (int)op);
	RtSession* s = lekmod_portable::InstanceSession(inst);
	RtUiReplica& rep = s->UiReplica();
	Out o(out, n_out);

	switch (op)
	{
	case GC_UI_OP_SELECT_UNIT:
	{
		if (!Need(n_in, 2, err, op)) return GC_ERR_INVALID_ARG;
		CvUnit* u = in[0] >= 0 && in[0] < MAX_PLAYERS ? GET_PLAYER((PlayerTypes)in[0]).getUnit(in[1]) : NULL;
		if (!u) return Fail(err, GC_ERR_NOT_FOUND, "gc_ui_op SELECT_UNIT: no unit (%d, %d)", (int)in[0], (int)in[1]);
		rep.SelectUnit(u, n_in >= 3 ? in[2] != 0 : true, n_in >= 4 ? in[3] != 0 : false);
		break;
	}
	case GC_UI_OP_SELECT_GROUP:
	{
		if (!Need(n_in, 2, err, op)) return GC_ERR_INVALID_ARG;
		CvUnit* u = in[0] >= 0 && in[0] < MAX_PLAYERS ? GET_PLAYER((PlayerTypes)in[0]).getUnit(in[1]) : NULL;
		if (!u) return Fail(err, GC_ERR_NOT_FOUND, "gc_ui_op SELECT_GROUP: no unit (%d, %d)", (int)in[0], (int)in[1]);
		rep.SelectGroup(u, n_in >= 3 && in[2] != 0, n_in >= 4 && in[3] != 0, n_in >= 5 && in[4] != 0);
		break;
	}
	case GC_UI_OP_HEAD_SELECTED_UNIT:
	{
		CvUnit* u = rep.HeadSelectedUnit();
		o.Put(u ? (int32_t)u->getOwner() : -1);
		o.Put(u ? u->GetID() : -1);
		break;
	}
	case GC_UI_OP_SELECTED_UNITS:
	{
		rep.HeadSelectedUnit();  // drops the dead
		const std::vector<IDInfo>& v = rep.Selected();
		for (size_t i = 0; i < v.size(); ++i)
		{
			o.Put((int32_t)v[i].eOwner);
			o.Put(v[i].iID);
		}
		break;
	}
	case GC_UI_OP_CLEAR_SELECTION: rep.ClearSelection(); break;
	case GC_UI_OP_LAST_SELECTED_UNIT:
	{
		CvUnit* u = rep.LastSelectedUnit();
		o.Put(u ? (int32_t)u->getOwner() : -1);
		o.Put(u ? u->GetID() : -1);
		break;
	}
	case GC_UI_OP_SELECT_CITY:
	{
		if (!Need(n_in, 2, err, op)) return GC_ERR_INVALID_ARG;
		CvCity* c = in[0] >= 0 && in[0] < MAX_PLAYERS ? GET_PLAYER((PlayerTypes)in[0]).getCity(in[1]) : NULL;
		if (!c) return Fail(err, GC_ERR_NOT_FOUND, "gc_ui_op SELECT_CITY: no city (%d, %d)", (int)in[0], (int)in[1]);
		rep.SelectCity(c);
		break;
	}
	case GC_UI_OP_HEAD_SELECTED_CITY:
	{
		CvCity* c = rep.HeadSelectedCity();
		o.Put(c ? (int32_t)c->getOwner() : -1);
		o.Put(c ? c->GetID() : -1);
		break;
	}
	case GC_UI_OP_CLEAR_SELECTED_CITIES: rep.ClearSelectedCities(); break;
	case GC_UI_OP_DO_SELECT_CITY_AT_PLOT:
	{
		if (!Need(n_in, 2, err, op)) return GC_ERR_INVALID_ARG;
		CvPlot* plot = GC.getMap().plot(in[0], in[1]);
		CvCity* c = plot ? plot->getPlotCity() : NULL;
		const bool ok = c && (rep.Seat() == NO_PLAYER || c->getOwner() == rep.Seat());
		if (ok) rep.SelectCity(c);
		o.Put(ok ? 1 : 0);
		break;
	}
	case GC_UI_OP_SET_INTERFACE_MODE:
		if (!Need(n_in, 1, err, op)) return GC_ERR_INVALID_ARG;
		rep.SetInterfaceMode((InterfaceModeTypes)in[0]);
		break;
	case GC_UI_OP_GET_INTERFACE_MODE: o.Put((int32_t)rep.InterfaceMode()); break;
	case GC_UI_OP_SET_INTERFACE_MODE_VALUE:
		if (!Need(n_in, 1, err, op)) return GC_ERR_INVALID_ARG;
		rep.SetInterfaceModeValue(in[0]);
		break;
	case GC_UI_OP_GET_INTERFACE_MODE_VALUE: o.Put(rep.InterfaceModeValue()); break;
	case GC_UI_OP_CAN_DO_INTERFACE_MODE:
		if (!Need(n_in, 1, err, op)) return GC_ERR_INVALID_ARG;
		o.Put(rep.CanDoInterfaceMode((InterfaceModeTypes)in[0], n_in >= 2 && in[1] != 0) ? 1 : 0);
		break;
	case GC_UI_OP_SET_CITY_SCREEN_UP:
		if (!Need(n_in, 1, err, op)) return GC_ERR_INVALID_ARG;
		rep.SetCityScreenUp(in[0] != 0);
		break;
	case GC_UI_OP_IS_CITY_SCREEN_UP: o.Put(rep.CityScreenUp() ? 1 : 0); break;
	case GC_UI_OP_CAN_SELECTION_LIST_FOUND: o.Put(rep.CanSelectionListFound() ? 1 : 0); break;
	case GC_UI_OP_CAN_SELECTION_LIST_WORK: o.Put(rep.CanSelectionListWork() ? 1 : 0); break;
	case GC_UI_OP_CAN_END_TURN: o.Put(s->UI().canEndTurn() ? 1 : 0); break;
	case GC_UI_OP_ACTIVATE_NOTIFICATION:
	{
		if (!Need(n_in, 1, err, op)) return GC_ERR_INVALID_ARG;
		if (rep.Seat() == NO_PLAYER) return Fail(err, GC_ERR_NOT_FOUND, "gc_ui_op ACTIVATE_NOTIFICATION: a spectator has no notifications");
		lekmod_portable::RtNotification n;
		if (!s->UI().FindNotificationOf(rep.Seat(), in[0], &n)) return Fail(err, GC_ERR_NOT_FOUND, "gc_ui_op ACTIVATE_NOTIFICATION: no notification %d", (int)in[0]);
		CvNotifications* core = GET_PLAYER(rep.Seat()).GetNotifications();
		if (core) core->Activate(in[0]);  // popup / camera / selection come out of the sink
		break;
	}
	case GC_UI_OP_REMOVE_NOTIFICATION:
	{
		if (!Need(n_in, 1, err, op)) return GC_ERR_INVALID_ARG;
		if (rep.Seat() == NO_PLAYER) return Fail(err, GC_ERR_NOT_FOUND, "gc_ui_op REMOVE_NOTIFICATION: a spectator has no notifications");
		std::map<int, lekmod_portable::RtNotification>& seen = rep.SeenNotifications();
		if (seen.find(in[0]) == seen.end()) return Fail(err, GC_ERR_NOT_FOUND, "gc_ui_op REMOVE_NOTIFICATION: no notification %d", (int)in[0]);
		seen.erase(in[0]);
		rep.HiddenNotifications().insert(in[0]);
		lekmod_portable::RtUiEv ev(GC_UI_EV_NOTIFICATION_REMOVED);
		ev.A(in[0]).A((int)rep.Seat());
		rep.Send(ev);
		break;
	}
	case GC_UI_OP_GET_CYCLE_SELECTION_COUNTER: o.Put(rep.CycleCounter()); break;
	case GC_UI_OP_SET_CYCLE_SELECTION_COUNTER:
		if (!Need(n_in, 1, err, op)) return GC_ERR_INVALID_ARG;
		rep.SetCycleCounter(in[0]);
		break;
	case GC_UI_OP_IS_UNIT_SELECTED:
		if (!Need(n_in, 2, err, op)) return GC_ERR_INVALID_ARG;
		o.Put(rep.IsUnitSelected((PlayerTypes)in[0], in[1]) ? 1 : 0);
		break;
	case GC_UI_OP_IS_CITY_SELECTED:
		if (!Need(n_in, 2, err, op)) return GC_ERR_INVALID_ARG;
		o.Put(rep.IsCitySelected((PlayerTypes)in[0], in[1]) ? 1 : 0);
		break;
	case GC_UI_OP_SELECT_ALL_AT_PLOT:
	{
		if (!Need(n_in, 2, err, op)) return GC_ERR_INVALID_ARG;
		CvPlot* plot = GC.getMap().plot(in[0], in[1]);
		if (!plot) return Fail(err, GC_ERR_NOT_FOUND, "gc_ui_op SELECT_ALL_AT_PLOT: no plot (%d, %d)", (int)in[0], (int)in[1]);
		rep.SelectAllAt(plot);
		break;
	}
	case GC_UI_OP_GET_SELECTION_COUNT:
		rep.HeadSelectedUnit();
		o.Put(rep.SelectionLength());
		break;
	default: return Fail(err, GC_ERR_INVALID_ARG, "gc_ui_op: unknown op %d", (int)op);
	}
	if (n_out) *n_out = o.n < o.cap ? o.n : o.cap;
	return Ok(err);
	GC_UI_GUARD_END(err)
}
