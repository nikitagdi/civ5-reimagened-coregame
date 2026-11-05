// See rt_ui_replica.h.
#include "CvGameCoreDLLPCH.h"

#include <algorithm>
#include <memory>

#include "CvDllCity.h"
#include "CvDllUnit.h"

#include "rt_active.h"
#include "rt_lua.h"
#include "rt_ui_replica.h"

namespace lekmod_portable {

namespace {

// The values of a GAME_EVENT / LUA_EVENT (gamecore_abi.h GC_UI_VAL_*).
struct ValueWriter
{
	std::vector<uint8_t> b;
	unsigned count;
	ValueWriter() : count(0) { b.push_back(0); }
	void U8(unsigned v) { b.push_back((uint8_t)v); }
	void U16(unsigned v)
	{
		b.push_back((uint8_t)(v & 0xFF));
		b.push_back((uint8_t)((v >> 8) & 0xFF));
	}
	void F64(double d)
	{
		uint8_t raw[8];
		memcpy(raw, &d, 8);  // little endian targets only (x86-64, wasm32)
		b.insert(b.end(), raw, raw + 8);
	}
	void Nil() { U8(GC_UI_VAL_NIL); ++count; }
	void Bool(bool v) { U8(GC_UI_VAL_BOOL); U8(v ? 1 : 0); ++count; }
	void Num(double d) { U8(GC_UI_VAL_NUM); F64(d); ++count; }
	void Str(const char* s, size_t len)
	{
		if (len > 0xFFFF) len = 0xFFFF;
		U8(GC_UI_VAL_STR);
		U16((unsigned)len);
		b.insert(b.end(), (const uint8_t*)s, (const uint8_t*)s + len);
		++count;
	}
	void Vec(const std::vector<double>& v)
	{
		U8(GC_UI_VAL_VEC);
		U8((unsigned)v.size());
		for (size_t i = 0; i < v.size(); ++i) F64(v[i]);
		++count;
	}
	void Finish() { b[0] = (uint8_t)(count > 255 ? 255 : count); }
};

PlayerTypes ViewPlayer(PlayerTypes seat)
{
	return seat != NO_PLAYER ? seat : CvPreGame::activePlayer();
}

CvUnit* FindUnit(PlayerTypes owner, int id)
{
	if (owner < 0 || owner >= MAX_PLAYERS || id < 0) return NULL;
	return GET_PLAYER(owner).getUnit(id);
}

CvCity* FindCity(PlayerTypes owner, int id)
{
	if (owner < 0 || owner >= MAX_PLAYERS || id < 0) return NULL;
	return GET_PLAYER(owner).getCity(id);
}

}  // namespace

RtUiReplica::RtUiReplica()
	: m_replica(false), m_seat(NO_PLAYER), m_rebuild(false), m_muted(false), m_mode(INTERFACEMODE_SELECTION), m_modeValue(0), m_cityScreenUp(false), m_cycle(0), m_revealing(false)
{
	m_sink.user = NULL;
	m_sink.event = NULL;
}

void RtUiReplica::SetSink(const gc_ui_sink* sink)
{
	if (sink)
		m_sink = *sink;
	else
	{
		m_sink.user = NULL;
		m_sink.event = NULL;
	}
}

void RtUiReplica::SetRole(bool replica, PlayerTypes seat)
{
	m_replica = replica;
	m_seat = replica ? seat : NO_PLAYER;
	if (!replica) ResetState();
}

void RtUiReplica::ResetState()
{
	m_selected.clear();
	m_last = IDInfo();
	m_cities.clear();
	m_mode = INTERFACEMODE_SELECTION;
	m_modeValue = 0;
	m_cityScreenUp = false;
	m_cycle = 0;
	m_revealing = false;
	m_seenNotes.clear();
	m_hiddenNotes.clear();
	m_fog.clear();
}

void RtUiReplica::Send(RtUiEv& ev)
{
	if (!Active()) return;
	if (m_rebuild) ev.e.flags |= GC_UI_F_REBUILD;
	m_sink.event(m_sink.user, &ev.e);
}

// ---- packing ---------------------------------------------------------------------------------------------------------------------

void RtUiReplica::PlotXY(const ICvPlot1* plot, int* x, int* y)
{
	*x = -1;
	*y = -1;
	if (plot) plot->GetPosition(*x, *y);
}

void RtUiReplica::PackUnit(RtUiEv& ev, ICvUnit1* unit)
{
	CvUnit* u = unit ? static_cast<CvDllUnit*>(unit)->GetInstance() : NULL;
	if (!u)
	{
		for (int i = 0; i < GC_UI_UNIT_INTS; ++i) ev.A(-1);
		return;
	}
	const PlayerTypes owner = u->getOwner();
	int primary = -1, secondary = -1;
	if (owner >= 0 && owner < MAX_PLAYERS)
	{
		CvPlayerColorInfo* color = GC.GetPlayerColorInfo(GET_PLAYER(owner).getPlayerColor());
		if (color)
		{
			primary = color->GetColorTypePrimary();
			secondary = color->GetColorTypeSecondary();
		}
	}
	// The reads of the seat's UI: its team is the active one for the call (the rules ran as the server seat).
	const PlayerTypes view = ViewPlayer(m_seat);
	ActiveAs as(view);
	CvPlot* plot = u->plot();
	const int fog = plot ? (int)plot->GetActiveFogOfWarMode() : (int)FOGOFWARMODE_UNEXPLORED;
	const TeamTypes viewTeam = view >= 0 && view < MAX_PLAYERS ? GET_PLAYER(view).getTeam() : NO_TEAM;
	ev.A(owner).A(u->GetID()).A(u->getX()).A(u->getY()).A((int)u->getUnitType());
	ev.A(owner >= 0 && owner < MAX_PLAYERS ? (int)GET_PLAYER(owner).getArtStyleType() : -1);
	ev.A((int)u->getCivilizationType()).A(primary).A(secondary).A(u->getUnitInfo().GetUnitFlagIconOffset()).A(fog);
	ev.A(IsUnitSelected(owner, u->GetID()) ? 1 : 0).A(u->IsCombatUnit() ? 1 : 0).A(viewTeam != NO_TEAM && u->isInvisible(viewTeam, false) ? 0 : 1);
}

void RtUiReplica::PackCity(RtUiEv& ev, ICvCity1* city)
{
	CvCity* c = city ? static_cast<CvDllCity*>(city)->GetInstance() : NULL;
	if (!c)
	{
		for (int i = 0; i < GC_UI_CITY_INTS; ++i) ev.A(-1);
		return;
	}
	ev.A(c->getOwner()).A(c->GetID()).A(c->getX()).A(c->getY()).A(c->getPopulation()).A(c->getDamage()).A(c->GetMaxHitPoints());
	ev.A((int)c->getCitySizeType()).A((int)c->getTeam()).A(c->IsPuppet() ? 1 : 0).A(c->isCapital() ? 1 : 0);
	ev.T(c->getNameKey());
}

void RtUiReplica::PackCombat(RtUiEv& ev, ICvCombatInfo1* info)
{
	if (!info) return;
	for (int side = 0; side < BATTLE_UNIT_COUNT; ++side)
	{
		std::auto_ptr<ICvUnit1> unit(info->GetUnit((BattleUnitTypes)side));
		std::auto_ptr<ICvCity1> city(info->GetCity((BattleUnitTypes)side));
		if (unit.get())
			ev.A(1).A(unit->GetOwner()).A(unit->GetID());
		else if (city.get())
			ev.A(2).A(city->GetOwner()).A(city->GetID());
		else
			ev.A(0).A(-1).A(-1);
	}
	std::auto_ptr<ICvPlot1> plot(info->GetPlot());
	int x, y;
	PlotXY(plot.get(), &x, &y);
	ev.A(x).A(y);
	ev.A((info->GetAttackerAdvances() ? 1 : 0) | (info->GetVisualizeCombat() ? 2 : 0) | (info->GetDefenderRetaliates() ? 4 : 0) | (info->GetAttackIsRanged() ? 8 : 0));
	ev.A(info->GetNuclearDamageLevel());
}

// ---- Lua events ------------------------------------------------------------------------------------------------------------------

void RtUiReplica::EmitGameEvent(const char* name, RtScriptArgs* args)
{
	if (!Active() || !Subscribed(name)) return;
	ValueWriter w;
	const unsigned n = args ? args->Count() : 0;
	for (unsigned i = 0; i < n; ++i)
	{
		bool b = false;
		int iv = 0;
		float f = 0.0f;
		char* s = NULL;
		switch (args->GetType(i))
		{
		case ICvEngineScriptSystemArgs1::ARGTYPE_BOOL: args->GetBool(i, b); w.Bool(b); break;
		case ICvEngineScriptSystemArgs1::ARGTYPE_INT: args->GetInt(i, iv); w.Num((double)iv); break;
		case ICvEngineScriptSystemArgs1::ARGTYPE_FLOAT: args->GetFloat(i, f); w.Num((double)f); break;
		case ICvEngineScriptSystemArgs1::ARGTYPE_STRING:
			if (args->GetString(i, s) && s) w.Str(s, strlen(s)); else w.Nil();
			break;
		default: w.Nil(); break;
		}
	}
	w.Finish();
	RtUiEv ev(GC_UI_EV_GAME_EVENT);
	ev.T(name).D(&w.b[0], w.b.size());
	Send(ev);
}

void RtUiReplica::EmitLuaEvent(const char* name, lua_State* L, int first, int last) { EmitLuaValues(GC_UI_EV_LUA_EVENT, name, L, first, last); }

void RtUiReplica::EmitLuaValues(int kind, const char* name, lua_State* L, int first, int last)
{
	if (!Active() || (kind == GC_UI_EV_GAME_EVENT && !Subscribed(name))) return;
	ValueWriter w;
	for (int i = first; i <= last; ++i)
	{
		switch (lua_type(L, i))
		{
		case LUA_TBOOLEAN: w.Bool(lua_toboolean(L, i) != 0); break;
		case LUA_TNUMBER: w.Num((double)lua_tonumber(L, i)); break;
		case LUA_TSTRING:
		{
			size_t len = 0;
			const char* s = lua_tolstring(L, i, &len);
			w.Str(s, len);
			break;
		}
		case LUA_TTABLE:
		{
			// A flat table of numbers: its array part, else the fields x, y, z (a hex position, HexToWorld's vector).
			std::vector<double> v;
			const int n = (int)lua_objlen(L, i);
			for (int k = 1; k <= n && v.size() < 255; ++k)
			{
				lua_rawgeti(L, i, k);
				if (lua_type(L, -1) == LUA_TNUMBER) v.push_back((double)lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
			if (v.empty())
			{
				static const char* const fields[3] = {"x", "y", "z"};
				for (int k = 0; k < 3; ++k)
				{
					lua_getfield(L, i, fields[k]);
					if (lua_type(L, -1) == LUA_TNUMBER) v.push_back((double)lua_tonumber(L, -1));
					lua_pop(L, 1);
				}
			}
			w.Vec(v);
			break;
		}
		default: w.Nil(); break;
		}
	}
	w.Finish();
	RtUiEv ev(kind);
	ev.T(name).D(&w.b[0], w.b.size());
	Send(ev);
}

// ---- selection -------------------------------------------------------------------------------------------------------------------

bool RtUiReplica::IsUnitSelected(PlayerTypes owner, int id) const
{
	for (size_t i = 0; i < m_selected.size(); ++i)
		if (m_selected[i].eOwner == owner && m_selected[i].iID == id) return true;
	return false;
}

void RtUiReplica::SelectionChanged()
{
	if (!Active()) return;
	RtUiEv ev(GC_UI_EV_UNIT_SELECTION_CHANGED);
	ev.A((int)m_selected.size());
	if (m_selected.empty())
		ev.A(-1).A(-1);
	else
		ev.A(m_selected[0].eOwner).A(m_selected[0].iID);
	Send(ev);
}

void RtUiReplica::CitySelectionChanged()
{
	if (!Active()) return;
	RtUiEv ev(GC_UI_EV_CITY_SELECTION_CHANGED);
	ev.A((int)m_cities.size());
	if (m_cities.empty())
		ev.A(-1).A(-1);
	else
		ev.A(m_cities[0].eOwner).A(m_cities[0].iID);
	Send(ev);
}

// The exe lets its player select only their own units.
void RtUiReplica::SelectUnit(CvUnit* unit, bool clear, bool toggle)
{
	if (!m_replica || !unit || (m_seat != NO_PLAYER && unit->getOwner() != m_seat)) return;
	const IDInfo who(unit->getOwner(), unit->GetID());
	bool changed = false;
	const bool present = IsUnitSelected(who.eOwner, who.iID);
	if (toggle && present)
	{
		m_selected.erase(std::find(m_selected.begin(), m_selected.end(), who));
		changed = true;
	}
	else
	{
		if (clear && !(m_selected.size() == 1 && present))
		{
			changed = !m_selected.empty();
			m_selected.clear();
		}
		if (!IsUnitSelected(who.eOwner, who.iID))
		{
			m_selected.push_back(who);
			changed = true;
		}
		m_last = who;
	}
	if (changed) SelectionChanged();
}

void RtUiReplica::SelectGroup(CvUnit* unit, bool shift, bool ctrl, bool alt)
{
	if (!m_replica || !unit) return;
	if (!shift) ClearSelection();
	// This version of the game has no unit groups: ctrl selects the units of the same type on the plot, alt every unit of the player there.
	std::vector<CvUnit*> pick;
	pick.push_back(unit);
	CvPlot* plot = unit->plot();
	if ((ctrl || alt) && plot)
	{
		const IDInfo* node = plot->headUnitNode();
		while (node)
		{
			CvUnit* other = FindUnit(node->eOwner, node->iID);
			node = plot->nextUnitNode(node);
			if (!other || other == unit || other->getOwner() != unit->getOwner()) continue;
			if (alt || other->getUnitType() == unit->getUnitType()) pick.push_back(other);
		}
	}
	for (size_t i = 0; i < pick.size(); ++i) SelectUnit(pick[i], false, false);
	m_last = IDInfo(unit->getOwner(), unit->GetID());
}

void RtUiReplica::SelectAllAt(CvPlot* plot)
{
	if (!m_replica || !plot) return;
	ClearSelection();
	const IDInfo* node = plot->headUnitNode();
	while (node)
	{
		CvUnit* u = FindUnit(node->eOwner, node->iID);
		node = plot->nextUnitNode(node);
		if (u) SelectUnit(u, false, false);
	}
}

bool RtUiReplica::RemoveFromSelection(PlayerTypes owner, int id)
{
	std::vector<IDInfo>::iterator it = std::find(m_selected.begin(), m_selected.end(), IDInfo(owner, id));
	if (it == m_selected.end()) return false;
	m_selected.erase(it);
	if (m_last == IDInfo(owner, id)) m_last = IDInfo();
	SelectionChanged();
	return true;
}

void RtUiReplica::ClearSelection()
{
	if (m_selected.empty()) return;
	m_selected.clear();
	SelectionChanged();
}

CvUnit* RtUiReplica::HeadSelectedUnit()
{
	while (!m_selected.empty())
	{
		CvUnit* u = FindUnit(m_selected[0].eOwner, m_selected[0].iID);
		if (u) return u;
		m_selected.erase(m_selected.begin());  // a unit that is gone is no selection
		SelectionChanged();
	}
	return NULL;
}

CvUnit* RtUiReplica::LastSelectedUnit() { return FindUnit(m_last.eOwner, m_last.iID); }

bool RtUiReplica::CanSelectionListFound()
{
	for (size_t i = 0; i < m_selected.size(); ++i)
	{
		CvUnit* u = FindUnit(m_selected[i].eOwner, m_selected[i].iID);
		if (u && u->isFound()) return true;
	}
	return false;
}

bool RtUiReplica::CanSelectionListWork()
{
	for (size_t i = 0; i < m_selected.size(); ++i)
	{
		CvUnit* u = FindUnit(m_selected[i].eOwner, m_selected[i].iID);
		if (u && u->IsWork()) return true;
	}
	return false;
}

// ---- cities, mode ----------------------------------------------------------------------------------------------------------------

void RtUiReplica::SelectCity(CvCity* city)
{
	if (!m_replica) return;
	if (!city)
	{
		ClearSelectedCities();
		return;
	}
	if (m_seat != NO_PLAYER && city->getOwner() != m_seat) return;
	const IDInfo who(city->getOwner(), city->GetID());
	if (m_cities.size() == 1 && m_cities[0] == who) return;
	m_cities.clear();
	m_cities.push_back(who);
	CitySelectionChanged();
}

void RtUiReplica::AddSelectedCity(CvCity* city, bool toggle)
{
	if (!m_replica || !city || (m_seat != NO_PLAYER && city->getOwner() != m_seat)) return;
	const IDInfo who(city->getOwner(), city->GetID());
	std::vector<IDInfo>::iterator it = std::find(m_cities.begin(), m_cities.end(), who);
	if (it != m_cities.end())
	{
		if (!toggle) return;
		m_cities.erase(it);
	}
	else
		m_cities.push_back(who);
	CitySelectionChanged();
}

void RtUiReplica::ClearSelectedCities()
{
	if (m_cities.empty()) return;
	m_cities.clear();
	CitySelectionChanged();
}

bool RtUiReplica::IsCitySelected(PlayerTypes owner, int id) const
{
	return std::find(m_cities.begin(), m_cities.end(), IDInfo(owner, id)) != m_cities.end();
}

CvCity* RtUiReplica::HeadSelectedCity()
{
	while (!m_cities.empty())
	{
		CvCity* c = FindCity(m_cities[0].eOwner, m_cities[0].iID);
		if (c) return c;
		m_cities.erase(m_cities.begin());
		CitySelectionChanged();
	}
	return NULL;
}

void RtUiReplica::SetCityScreenUp(bool up)
{
	if (m_cityScreenUp == up) return;
	m_cityScreenUp = up;
	if (Active())
	{
		RtUiEv ev(GC_UI_EV_CITY_SCREEN_CHANGED);
		ev.A(up ? 1 : 0);
		Send(ev);
	}
}

void RtUiReplica::SetInterfaceMode(InterfaceModeTypes mode)
{
	if (!m_replica) return;
	const InterfaceModeTypes old = m_mode;
	m_mode = mode;
	if (Active())
	{
		RtUiEv ev(GC_UI_EV_INTERFACE_MODE_CHANGED);
		ev.A((int)old).A((int)mode).A(m_modeValue);
		Send(ev);
	}
}

// CvInterface::canDoInterfaceMode: the selection mode is always possible, the city modes need the selected city, the others a selected unit that can do the mode.
bool RtUiReplica::CanDoInterfaceMode(InterfaceModeTypes mode, bool testVisible)
{
	if (mode == NO_INTERFACEMODE) return false;
	switch (mode)
	{
	case INTERFACEMODE_SELECTION:
	case INTERFACEMODE_PING:
	case INTERFACEMODE_DEBUG: return true;
	case INTERFACEMODE_PURCHASE_PLOT:
	case INTERFACEMODE_CITY_PLOT_SELECTION:
	case INTERFACEMODE_CITY_RANGE_ATTACK: return HeadSelectedCity() != NULL;
	default: break;
	}
	for (size_t i = 0; i < m_selected.size(); ++i)
	{
		CvUnit* u = FindUnit(m_selected[i].eOwner, m_selected[i].iID);
		if (u && u->CanDoInterfaceMode(mode, testVisible)) return true;
	}
	return false;
}

void RtUiReplica::SyncFog()
{
	if (!Active() || m_seat == NO_PLAYER) return;
	CvMap& map = GC.getMap();
	const int n = map.numPlots();
	if ((int)m_fog.size() != n) m_fog.assign((size_t)n, (unsigned char)FOGOFWARMODE_UNEXPLORED);
	ActiveAs as(m_seat);
	for (int i = 0; i < n; ++i)
	{
		CvPlot* plot = map.plotByIndexUnchecked(i);
		if (!plot) continue;
		const unsigned char mode = (unsigned char)plot->GetActiveFogOfWarMode();
		if (m_fog[(size_t)i] == mode) continue;
		m_fog[(size_t)i] = mode;
		RtUiEv ev(GC_UI_EV_FOW_CHANGED);
		ev.A(plot->getX()).A(plot->getY()).A((int)mode).A(0);
		Send(ev);
	}
}

void RtUiReplica::Revalidate()
{
	bool changed = false;
	for (size_t i = 0; i < m_selected.size();)
	{
		if (FindUnit(m_selected[i].eOwner, m_selected[i].iID)) { ++i; continue; }
		m_selected.erase(m_selected.begin() + i);
		changed = true;
	}
	if (changed) SelectionChanged();
	changed = false;
	for (size_t i = 0; i < m_cities.size();)
	{
		if (FindCity(m_cities[i].eOwner, m_cities[i].iID)) { ++i; continue; }
		m_cities.erase(m_cities.begin() + i);
		changed = true;
	}
	if (changed) CitySelectionChanged();
	if (!FindUnit(m_last.eOwner, m_last.iID)) m_last = IDInfo();
}

}  // namespace lekmod_portable
