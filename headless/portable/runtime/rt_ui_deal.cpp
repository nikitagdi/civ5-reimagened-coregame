// B4 M4 WO-D: see rt_ui_deal.h. Ported from headless/host/src/script/HstExeLua.cpp (the bot mode's UI.* deal functions).
#include "CvGameCoreDLLPCH.h"

#include "rt_lua.h"
#include "CvDealAI.h"
#include "CvDealClasses.h"
#include "CvDiplomacyAI.h"
#include "CvEconomicAI.h"
#include "CvAdvisorCounsel.h"
#include "CvTreasury.h"
#include "CvMinorCivAI.h"
#include "rt_engine.h"
#include "rt_ui_deal.h"
#include "../abi/gc_command.h"

namespace lekmod_portable {

namespace {

CvDeal* g_scratch = NULL;
ICvScriptSystemUtility1* g_libs = NULL;
RtEngine* g_engine = NULL;

inline PlayerTypes Active() { return GC.getGame().getActivePlayer(); }

// The AI major the scratch deal is with, or NULL (a human or nobody; a deal with a human is a proposal, see lProposeDeal).
CvPlayer* AiOf(PlayerTypes p)
{
	if (p < 0 || p >= MAX_MAJOR_CIVS) return NULL;
	CvPlayer& pl = GET_PLAYER(p);
	return (pl.isAlive() && !pl.isHuman()) ? &pl : NULL;
}

int lGetScratchDeal(lua_State* L)
{
	std::auto_ptr<ICvDeal1> d = GC.WrapDealPointer(RtUiScratchDeal());
	if (!g_libs) { lua_pushnil(L); return 1; }
	g_libs->PushCvDealInstance(L, d.get());
	return 1;
}

// A living human major other than the active player (the other side of a deal between humans), or NULL.
CvPlayer* HumanOf(PlayerTypes p)
{
	if (p < 0 || p >= MAX_MAJOR_CIVS || p == Active()) return NULL;
	CvPlayer& pl = GET_PLAYER(p);
	return (pl.isAlive() && pl.isHuman()) ? &pl : NULL;
}

// UI.DoProposeDeal: the AI answers; accepted, it goes out as sendNetDealAccepted. Returns the response type (-1: nobody to answer).
// B4 fix-mp-trade: to a human the deal goes out as a proposal (the exe's NetPlayerDealProposed); the other side answers with UI.DoFinalizePlayerDeal.
int lProposeDeal(lua_State* L)
{
	CvDeal* d = RtUiScratchDeal();
	if (g_engine && d->GetFromPlayer() == Active() && HumanOf(d->GetToPlayer()) && d->GetNumItems() > 0)
	{
		std::auto_ptr<ICvDeal1> w = GC.WrapDealPointer(d);
		g_engine->SendNetDealProposed(Active(), d->GetToPlayer(), w.get());
		lua_pushinteger(L, -1);
		return 1;
	}
	CvPlayer* ai = AiOf(d->GetToPlayer());
	lua_pushinteger(L, ai ? (int)ai->GetDealAI()->DoHumanOfferDealToThisAI(d) : -1);
	return 1;
}

// UI.DoFinalizePlayerDeal(from, to, accepted): the receiver accepts or refuses a proposal, the proposer withdraws it (the exe's NetPlayerDealFinalized).
int lFinalizePlayerDeal(lua_State* L)
{
	const PlayerTypes from = (PlayerTypes)luaL_checkinteger(L, 1), to = (PlayerTypes)luaL_checkinteger(L, 2);
	const bool accepted = lua_toboolean(L, 3) != 0;
	const PlayerTypes us = Active();
	if (!g_engine || (us != from && us != to) || from < 0 || from >= MAX_MAJOR_CIVS || to < 0 || to >= MAX_MAJOR_CIVS) return 0;
	if (!GC.getGame().GetGameDeals()->ProposedDealExists(from, to)) return 0;
	g_engine->SendNetPlayerDealFinalized(from, to, accepted && us == to);
	return 0;
}

// UI.HasMadeProposal(player): the player the player's open proposal goes to, -1 for none (CvGameDeals::HasMadeProposal).
int lHasMadeProposal(lua_State* L)
{
	const PlayerTypes p = (PlayerTypes)luaL_checkinteger(L, 1);
	lua_pushinteger(L, (p >= 0 && p < MAX_MAJOR_CIVS) ? (int)GC.getGame().GetGameDeals()->HasMadeProposal(p) : -1);
	return 1;
}

int lDemand(lua_State* L)
{
	CvDeal* d = RtUiScratchDeal();
	CvPlayer* ai = AiOf(d->GetToPlayer());
	lua_pushinteger(L, ai ? (int)ai->GetDealAI()->DoHumanDemand(d) : -1);
	return 1;
}

// mode 0 UI.DoEqualizeDealWithHuman, 1 UI.DoWhatDoesAIWant (the AI's items stay), 2 UI.DoWhatWillAIGive (the human's items stay).
int lEqualize(lua_State* L)
{
	const int mode = (int)luaL_optinteger(L, 1, 0);
	CvDeal* d = RtUiScratchDeal();
	CvPlayer* ai = AiOf(d->GetToPlayer());
	bool good = false, cant = false, ok = false;
	if (ai) ok = ai->GetDealAI()->DoEqualizeDealWithHuman(d, d->GetFromPlayer(), mode == 1, mode == 2, good, cant);
	lua_pushboolean(L, ok);
	return 1;
}

int lProposedDealExists(lua_State* L)
{
	const PlayerTypes from = (PlayerTypes)luaL_checkinteger(L, 1), to = (PlayerTypes)luaL_checkinteger(L, 2);
	lua_pushboolean(L, from >= 0 && from < MAX_MAJOR_CIVS && to >= 0 && to < MAX_MAJOR_CIVS && GC.getGame().GetGameDeals()->ProposedDealExists(from, to));
	return 1;
}

int lLoadProposedDeal(lua_State* L)
{
	const PlayerTypes from = (PlayerTypes)luaL_checkinteger(L, 1), to = (PlayerTypes)luaL_checkinteger(L, 2);
	if (from < 0 || from >= MAX_MAJOR_CIVS || to < 0 || to >= MAX_MAJOR_CIVS) return 0;
	CvDeal* p = GC.getGame().GetGameDeals()->GetProposedDeal(from, to);
	if (p) *RtUiScratchDeal() = *p;
	return 0;
}

// UI.GetNumCurrentDeals / GetNumHistoricDeals (player): the running and the ended deals the player is in (CvGameDeals).
int lGetNumDeals(lua_State* L, bool historic)
{
	const PlayerTypes p = (PlayerTypes)luaL_checkinteger(L, 1);
	CvGameDeals* deals = GC.getGame().GetGameDeals();
	lua_pushinteger(L, (p < 0 || p >= MAX_MAJOR_CIVS) ? 0 : (int)(historic ? deals->GetNumHistoricDeals(p) : deals->GetNumCurrentDeals(p)));
	return 1;
}
int lGetNumCurrentDeals(lua_State* L) { return lGetNumDeals(L, false); }
int lGetNumHistoricDeals(lua_State* L) { return lGetNumDeals(L, true); }

// UI.LoadCurrentDeal / LoadHistoricDeal (player, index): the deal into the scratch deal (the Diplomatic Overview, the top panel's
// resource and gold lines and the war declaration read the deals this way). An index out of range leaves the scratch deal alone.
int lLoadDeal(lua_State* L, bool historic)
{
	const PlayerTypes p = (PlayerTypes)luaL_checkinteger(L, 1);
	const int index = (int)luaL_checkinteger(L, 2);
	if (p < 0 || p >= MAX_MAJOR_CIVS || index < 0) return 0;
	CvGameDeals* deals = GC.getGame().GetGameDeals();
	CvDeal* d = historic ? deals->GetHistoricDeal(p, (uint)index) : deals->GetCurrentDeal(p, (uint)index);
	if (d) *RtUiScratchDeal() = *d;
	return 0;
}
int lLoadCurrentDeal(lua_State* L) { return lLoadDeal(L, false); }
int lLoadHistoricDeal(lua_State* L) { return lLoadDeal(L, true); }

// Headless read-only snapshots. Explicit seats never borrow Active() or the UI scratch deal.
PlayerTypes ReadSeat(lua_State* L, int arg)
{
	const int p = (int)luaL_checkinteger(L, arg);
	if (p < 0 || p >= MAX_MAJOR_CIVS || !GET_PLAYER((PlayerTypes)p).isEverAlive())
		luaL_argerror(L, arg, "expected an initialized major player");
	return (PlayerTypes)p;
}

void IntField(lua_State* L, const char* key, int value)
{
	lua_pushinteger(L, value);
	lua_setfield(L, -2, key);
}

void PushDealSnapshot(lua_State* L, const CvDeal* d)
{
	if (!d) { lua_pushnil(L); return; }
	lua_newtable(L);
	IntField(L, "from", d->m_eFromPlayer);
	IntField(L, "to", d->m_eToPlayer);
	IntField(L, "start_turn", d->m_iStartTurn);
	IntField(L, "final_turn", d->m_iFinalTurn);
	IntField(L, "duration", d->m_iDuration);
	lua_newtable(L);
	int index = 0;
	for (TradedItemList::const_iterator it = d->m_TradedItems.begin(); it != d->m_TradedItems.end(); ++it)
	{
		lua_newtable(L);
		IntField(L, "type", it->m_eItemType);
		IntField(L, "from", it->m_eFromPlayer);
		IntField(L, "data1", it->m_iData1);
		IntField(L, "data2", it->m_iData2);
		IntField(L, "data3", it->m_iData3);
		IntField(L, "duration", it->m_iDuration);
#ifdef AUI_YIELDS_APPLIED_AFTER_TURN_NOT_BEFORE
		const int left = it->m_iTurnsRemaining;
#else
		const int left = it->m_iFinalTurn - GC.getGame().getGameTurn();
#endif
		IntField(L, "turns_left", left > 0 ? left : 0);
		lua_pushboolean(L, it->m_bFlag1);
		lua_setfield(L, -2, "flag1");
		lua_rawseti(L, -2, ++index);
	}
	lua_setfield(L, -2, "items");
}

int lReadDeals(lua_State* L, bool historic)
{
	const PlayerTypes seat = ReadSeat(L, 1);
	CvGameDeals* deals = GC.getGame().GetGameDeals();
	lua_newtable(L);
	if (!deals) return 1;
	const uint count = historic ? deals->GetNumHistoricDeals(seat) : deals->GetNumCurrentDeals(seat);
	for (uint i = 0; i < count; ++i)
	{
		PushDealSnapshot(L, historic ? deals->GetHistoricDeal(seat, i) : deals->GetCurrentDeal(seat, i));
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}
int lReadCurrentDeals(lua_State* L) { return lReadDeals(L, false); }
int lReadHistoricDeals(lua_State* L) { return lReadDeals(L, true); }

int lReadProposedDeal(lua_State* L)
{
	const PlayerTypes seat = ReadSeat(L, 1);
	const PlayerTypes from = ReadSeat(L, 2), to = ReadSeat(L, 3);
	if (seat != from && seat != to) return luaL_error(L, "seat must be a party to the deal");
	CvGameDeals* deals = GC.getGame().GetGameDeals();
	PushDealSnapshot(L, deals ? deals->GetProposedDeal(from, to) : NULL);
	return 1;
}

int lReadAdvisorCounsel(lua_State* L)
{
	const PlayerTypes seat = ReadSeat(L, 1);
	// A local counsel avoids replacing the global advisor's cached list for another seat.
	CvAdvisorCounsel counsel;
	{
#if defined(LEKMOD_HEADLESS_FIX_0023)
		HEADLESS_UI_TREASURY_SCOPE();
#endif
		counsel.BuildCounselList(seat);
	}
	lua_newtable(L);
	int indices[NUM_ADVISOR_TYPES] = {0};
	for (uint i = 0; i < counsel.m_aCounsel.size(); ++i)
	{
		const CvAdvisorCounsel::Counsel& c = counsel.m_aCounsel[i];
		const int advisor = (int)c.m_eAdvisor;
		if (advisor < 0 || advisor >= NUM_ADVISOR_TYPES) continue;
		lua_rawgeti(L, -1, advisor);
		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			lua_newtable(L);
		}
		lua_pushstring(L, c.m_strTxtKey);
		lua_rawseti(L, -2, indices[advisor]++);
		lua_rawseti(L, -2, advisor);
	}
	return 1;
}

// GcUiGetBullyScores(player, minor): the tribute pressure of a major on a living city-state, for gold and for a worker
// (CvMinorCivAI::CalculateBullyMetric, the scores GetMajorBullyGoldDetails / GetMajorBullyUnitDetails print; tribute is
// possible at zero and above). The same calls those Lua getters make, without the tooltip text.
int lReadBullyScores(lua_State* L)
{
	const PlayerTypes seat = ReadSeat(L, 1);
	const int minor = luaL_checkinteger(L, 2);
	if (minor < MAX_MAJOR_CIVS || minor >= MAX_CIV_PLAYERS || !GET_PLAYER((PlayerTypes)minor).isMinorCiv() || !GET_PLAYER((PlayerTypes)minor).isAlive())
		return luaL_argerror(L, 2, "expected a living city-state");
	CvMinorCivAI* ai = GET_PLAYER((PlayerTypes)minor).GetMinorCivAI();
	lua_pushinteger(L, ai->CalculateBullyMetric(seat, false));
	lua_pushinteger(L, ai->CalculateBullyMetric(seat, true));
	return 2;
}

// The lazy might cache of a player (CvPlayer::GetMilitaryMight / GetEconomicMight refresh it for whoever asks first in a turn,
// DS-0034): pointers to the protected members, formed through a derived name; the core's class is unchanged.
struct MightPeek : CvPlayer
{
	typedef FAutoVariable<int, CvPlayer> CvPlayer::* Field;
	static Field Turn() { return &MightPeek::m_iTurnMightRecomputed; }
	static Field Military() { return &MightPeek::m_iMilitaryMight; }
	static Field Economic() { return &MightPeek::m_iEconomicMight; }
};

// What a screen script may change on the asking machine only, through Lua getters that refresh or draw on synced state:
// the game and map RNG (DS-0038: City:GetNextBuyablePlot / City:CanBuyPlot() draw from the jon stream), every player's
// might cache (DS-0034 through the bully metric: Player:CanMajorBullyGold / GetMajorBullyGoldDetails / GetMajorBullyUnitDetails,
// which FIX-0022 does not route through HeadlessUiMight) and every player's exploration plots (Unit:CanDoCommand(COMMAND_AUTOMATE,
// AUTOMATE_EXPLORE) -> CvUnit::CanAutomate -> CvHomelandAI::IsAnyValidExploreMoves -> CvEconomicAI::GetExplorationPlots, which
// runs UpdatePlots while m_bExplorationPlotsDirty is set). Restoring the saved values makes the read leave them as they were.
struct UiReadSnapshot
{
	CvRandom jon, map;
	int turn[MAX_PLAYERS], military[MAX_PLAYERS], economic[MAX_PLAYERS];
	// The exploration plots are rebuilt only while dirty, so only a dirty player's economic AI can change: it is saved with the
	// class's own Write (the lists, their serialized size, the dirty flag) and put back with its Read, which leaves the flag set,
	// so the simulation rebuilds them where it would have (UpdatePlots also rebuilds the unserialized explorer list).
	FMemoryStream* exploration[MAX_PLAYERS];
	UiReadSnapshot() : jon(GC.getGame().getJonRand()), map(GC.getGame().getMapRand())
	{
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			CvPlayer& p = GET_PLAYER((PlayerTypes)i);
			turn[i] = (p.*MightPeek::Turn()).get();
			military[i] = (p.*MightPeek::Military()).get();
			economic[i] = (p.*MightPeek::Economic()).get();
			exploration[i] = NULL;
			CvEconomicAI* ai = p.GetEconomicAI();
			if (ai && ai->m_bExplorationPlotsDirty)
			{
				exploration[i] = new FMemoryStream();
				ai->Write(*exploration[i]);
			}
		}
	}
	~UiReadSnapshot()
	{
		for (int i = 0; i < MAX_PLAYERS; ++i) delete exploration[i];
	}
	void Restore() const
	{
		if (!(GC.getGame().getJonRand() == jon)) GC.getGame().getJonRand() = jon;
		if (!(GC.getGame().getMapRand() == map)) GC.getGame().getMapRand() = map;
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			CvPlayer& p = GET_PLAYER((PlayerTypes)i);
			if ((p.*MightPeek::Turn()).get() != turn[i]) p.*MightPeek::Turn() = turn[i];
			if ((p.*MightPeek::Military()).get() != military[i]) p.*MightPeek::Military() = military[i];
			if ((p.*MightPeek::Economic()).get() != economic[i]) p.*MightPeek::Economic() = economic[i];
			CvEconomicAI* ai = p.GetEconomicAI();
			if (exploration[i] && ai && !ai->m_bExplorationPlotsDirty)
			{
				exploration[i]->Rewind();
				ai->Read(*exploration[i]);
			}
		}
	}

private:
	UiReadSnapshot(const UiReadSnapshot&);
	void operator=(const UiReadSnapshot&);
};

// GcUiReadScope(f, ...): calls f(...) and returns its results; whatever f changed of the state above is put back first, also
// when f raises (the error is raised again after the restore). Hosts run every screen query and action script through it.
int lReadScope(lua_State* L)
{
	luaL_checktype(L, 1, LUA_TFUNCTION);
	const int base = lua_gettop(L);
	UiReadSnapshot saved;
	const int rc = lua_pcall(L, base - 1, LUA_MULTRET, 0);
	saved.Restore();
	if (rc != 0) return lua_error(L);
	return lua_gettop(L);
}

// GcUiSetMoveQuestions(player, on): the host says whether this player's client answers BUTTONPOPUP_DECLAREWARMOVE, so that
// gc_apply_command may ask it before a move into a rival's land at peace (gc_command.h). Runtime-only, never saved, no game state.
int lSetMoveQuestions(lua_State* L)
{
	const PlayerTypes seat = ReadSeat(L, 1);
	luaL_checktype(L, 2, LUA_TBOOLEAN);
	SetMoveQuestions((int)seat, lua_toboolean(L, 2) != 0);
	return 0;
}

// UI.OnHumanOpenedTradeScreen / OnHumanDemand: the leader answers with the state the trade screen shows.
int SayState(lua_State* L, DiploUIStateTypes state, DiploMessageTypes msg)
{
	const PlayerTypes ai = (PlayerTypes)luaL_checkinteger(L, 1);
	if (!AiOf(ai)) return 0;
	const char* text = GET_PLAYER(ai).GetDiplomacyAI()->GetDiploStringForMessage(msg, Active());
	gDLL->GameplayDiplomacyAILeaderMessage(ai, state, text ? text : "", LEADERHEAD_ANIM_LETS_HEAR_IT, 0);
	return 0;
}
int lOpenedTrade(lua_State* L) { return SayState(L, DIPLO_UI_STATE_TRADE, DIPLO_MESSAGE_LETS_HEAR_IT); }
int lHumanDemand(lua_State* L) { return SayState(L, DIPLO_UI_STATE_HUMAN_DEMAND, DIPLO_MESSAGE_LETS_HEAR_IT); }

}  // namespace

CvDeal* RtUiScratchDeal()
{
	if (!g_scratch) g_scratch = new CvDeal();
	return g_scratch;
}

void RegisterUiDealLib(lua_State* L, ICvScriptSystemUtility1* libs, RtEngine* engine)
{
	g_libs = libs;
	g_engine = engine;
	static const luaL_Reg fns[] = {
		{"GcUiGetAdvisorCounsel", lReadAdvisorCounsel},
		{"GcUiGetCurrentDeals", lReadCurrentDeals},
		{"GcUiGetHistoricDeals", lReadHistoricDeals},
		{"GcUiGetProposedDeal", lReadProposedDeal},
		{"GcUiGetBullyScores", lReadBullyScores},
		{"GcUiReadScope", lReadScope},
		{"GcUiSetMoveQuestions", lSetMoveQuestions},
		{"GcUiGetScratchDeal", lGetScratchDeal},
		{"GcUiProposeDeal", lProposeDeal},
		{"GcUiDemand", lDemand},
		{"GcUiEqualize", lEqualize},
		{"GcUiProposedDealExists", lProposedDealExists},
		{"GcUiLoadProposedDeal", lLoadProposedDeal},
		{"GcUiOpenedTradeScreen", lOpenedTrade},
		{"GcUiHumanDemand", lHumanDemand},
		{"GcUiFinalizePlayerDeal", lFinalizePlayerDeal},
		{"GcUiHasMadeProposal", lHasMadeProposal},
		{"GcUiGetNumCurrentDeals", lGetNumCurrentDeals},
		{"GcUiGetNumHistoricDeals", lGetNumHistoricDeals},
		{"GcUiLoadCurrentDeal", lLoadCurrentDeal},
		{"GcUiLoadHistoricDeal", lLoadHistoricDeal},
		{NULL, NULL}};
	for (const luaL_Reg* r = fns; r->name; ++r) lua_register(L, r->name, r->func);
}

}  // namespace lekmod_portable
