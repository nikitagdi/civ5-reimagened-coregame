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
#include "rt_active.h"
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

// ---- Seat-scoped AI diplomacy of a server seat (leader screen, trade with an AI) -----------------------------------------------------------
// The original runs these on the human's own machine with the human as the active player: the leader's greeting (DoBeginDiploWithHuman ->
// GetGreetHumanMessage), the leader's lines (GetDiploStringForMessage) and the trade screen's valuation (UI.DoProposeDeal ->
// DoHumanOfferDealToThisAI's acceptance test, UI.DoEqualizeDealWithHuman / DoWhatDoesAIWant / DoWhatWillAIGive). A server answers every seat
// from one game: the seat is an explicit argument, the active player is that seat only inside the call (ActiveAs switches the pregame field,
// nothing else), and the deal is a local CvDeal built from the caller's plain item list, never the UI scratch deal. Nothing here changes the
// game: an accepted offer leaves as the deal blob of `GcUiDealQuery(..., "blob")`, which the screen sends as the seat's sendNetDealAccepted.
// No Lua error is raised while the active player is switched (the arguments are read first), so the switch always comes back.

int g_startDiploRepeat = 0;  // GetStartDiploRepeatCount while a greeting is built (the seat's own count, kept by its screen session)

struct StartRepeatScope
{
	explicit StartRepeatScope(int n) { g_startDiploRepeat = n; }
	~StartRepeatScope() { g_startDiploRepeat = 0; }
};

// A living AI major the seat has met.
PlayerTypes ReadAi(lua_State* L, int arg, PlayerTypes seat)
{
	const int p = (int)luaL_checkinteger(L, arg);
	if (p < 0 || p >= MAX_MAJOR_CIVS || p == (int)seat || !GET_PLAYER((PlayerTypes)p).isAlive() || GET_PLAYER((PlayerTypes)p).isHuman())
		luaL_argerror(L, arg, "expected a living AI major civilization");
	if (!GET_TEAM(GET_PLAYER(seat).getTeam()).isHasMet(GET_PLAYER((PlayerTypes)p).getTeam()))
		luaL_argerror(L, arg, "the seat has not met this civilization");
	return (PlayerTypes)p;
}

// A living human seat (the one whose screen asks).
PlayerTypes ReadHumanSeat(lua_State* L, int arg)
{
	const PlayerTypes seat = ReadSeat(L, arg);
	if (!GET_PLAYER(seat).isAlive() || !GET_PLAYER(seat).isHuman()) luaL_argerror(L, arg, "expected a living human seat");
	return seat;
}

// GcUiLeaderGreeting(seat, ai, repeat): the greeting of CvDiplomacyAI::DoBeginDiploWithHuman for this seat, and its animation;
// `repeat` is the seat's own start-diplomacy count (UI.ChangeStartDiploRepeatCount of the original's leader list). nil under Always War.
int lLeaderGreeting(lua_State* L)
{
	const PlayerTypes seat = ReadHumanSeat(L, 1);
	const PlayerTypes ai = ReadAi(L, 2, seat);
	const int repeat = (int)luaL_optinteger(L, 3, 0);
	if (GC.getGame().isOption(GAMEOPTION_ALWAYS_WAR))
	{
		lua_pushnil(L);
		return 1;
	}
	std::string text;
	LeaderheadAnimationTypes anim = LEADERHEAD_ANIM_NEUTRAL_HELLO;
	{
		ActiveAs as(seat);
		StartRepeatScope counts(repeat);
		const char* s = GET_PLAYER(ai).GetDiplomacyAI()->GetGreetHumanMessage(anim);
		text = s ? s : "";
	}
	lua_pushstring(L, text.c_str());
	lua_pushinteger(L, (int)anim);
	return 2;
}

struct NamedDiploMessage
{
	const char* name;
	DiploMessageTypes message;
};
const NamedDiploMessage kDiploMessages[] = {
	{"LETS_HEAR_IT", DIPLO_MESSAGE_LETS_HEAR_IT},
	{"TRADE_REJECT_UNACCEPTABLE", DIPLO_MESSAGE_TRADE_REJECT_UNACCEPTABLE},
	{"TRADE_REJECT_INSULTING", DIPLO_MESSAGE_TRADE_REJECT_INSULTING},
	{"REPEAT_TRADE", DIPLO_MESSAGE_REPEAT_TRADE},
	{"REPEAT_TRADE_TOO_MUCH", DIPLO_MESSAGE_REPEAT_TRADE_TOO_MUCH},
	{"TRADE_DEAL_UNCHANGED", DIPLO_MESSAGE_TRADE_DEAL_UNCHANGED},
	{"TRADE_NO_DEAL_POSSIBLE", DIPLO_MESSAGE_TRADE_NO_DEAL_POSSIBLE},
	{"TRADE_CANT_MATCH_OFFER", DIPLO_MESSAGE_TRADE_CANT_MATCH_OFFER},
	{"TRADE_AI_MAKES_OFFER", DIPLO_MESSAGE_TRADE_AI_MAKES_OFFER},
	{"PEACE_WHAT_WILL_HUMAN_OFFER", DIPLO_MESSAGE_PEACE_WHAT_WILL_HUMAN_OFFER},
	{"NO_PEACE", DIPLO_MESSAGE_NO_PEACE},
	{"TOO_SOON_NO_PEACE", DIPLO_MESSAGE_TOO_SOON_NO_PEACE},
	{NULL, NO_DIPLO_MESSAGE_TYPE}};

// GcUiDiploText(seat, ai, name): the leader's line for one of the messages above (GetDiploStringForMessage for the seat).
int lDiploText(lua_State* L)
{
	const PlayerTypes seat = ReadHumanSeat(L, 1);
	const PlayerTypes ai = ReadAi(L, 2, seat);
	const char* name = luaL_checkstring(L, 3);
	DiploMessageTypes message = NO_DIPLO_MESSAGE_TYPE;
	for (const NamedDiploMessage* m = kDiploMessages; m->name; ++m)
		if (strcmp(m->name, name) == 0) message = m->message;
	if (message == NO_DIPLO_MESSAGE_TYPE) return luaL_argerror(L, 3, "unknown leader message");
	std::string text;
	{
		ActiveAs as(seat);
		const char* s = GET_PLAYER(ai).GetDiplomacyAI()->GetDiploStringForMessage(message, seat);
		text = s ? s : "";
	}
	lua_pushstring(L, text.c_str());
	return 1;
}

// One item of a draft: what the trade screen holds (TradeLogic's g_Deal rows).
struct DraftItem
{
	int type, from, data1, data2, data3;
	bool flag1;
};

int IntOf(lua_State* L, int table, const char* key, int fallback, bool* ok)
{
	lua_getfield(L, table, key);
	int value = fallback;
	if (lua_isnumber(L, -1))
	{
		const lua_Number n = lua_tonumber(L, -1);
		if (n != (lua_Number)(int)n) *ok = false;
		value = (int)n;
	}
	else if (!lua_isnil(L, -1))
		*ok = false;
	lua_pop(L, 1);
	return value;
}

// The items argument: a list of {type, from, data1, data2, data3, flag1}; `from` must be one of the two parties.
void ReadDraft(lua_State* L, int arg, PlayerTypes a, PlayerTypes b, std::vector<DraftItem>& out)
{
	luaL_checktype(L, arg, LUA_TTABLE);
	const int n = (int)lua_objlen(L, arg);
	if (n > 64) luaL_argerror(L, arg, "too many deal items");
	for (int i = 1; i <= n; ++i)
	{
		lua_rawgeti(L, arg, i);
		if (!lua_istable(L, -1)) luaL_argerror(L, arg, "a deal item is a table");
		const int t = lua_gettop(L);
		bool ok = true;
		DraftItem it;
		it.type = IntOf(L, t, "type", -1, &ok);
		it.from = IntOf(L, t, "from", -1, &ok);
		it.data1 = IntOf(L, t, "data1", 0, &ok);
		it.data2 = IntOf(L, t, "data2", 0, &ok);
		it.data3 = IntOf(L, t, "data3", 0, &ok);
		lua_getfield(L, t, "flag1");
		it.flag1 = lua_toboolean(L, -1) != 0;
		lua_pop(L, 2);
		if (!ok || (it.from != (int)a && it.from != (int)b) || it.type < 0 || it.type >= NUM_TRADEABLE_ITEMS)
			luaL_argerror(L, arg, "invalid deal item");
		out.push_back(it);
	}
}

// Adds one item the way the trade screen does (TradeLogic: the deal's own Add* calls, which test IsPossibleToTradeItem against what the
// deal holds already). False: the core refuses it, or the item is not one this table supports.
bool AddDraftItem(CvDeal& d, const DraftItem& it)
{
	const PlayerTypes from = (PlayerTypes)it.from;
	const int duration = GC.getGame().GetDealDuration();
	const int before = d.GetNumItems();
	switch (it.type)
	{
	case TRADE_ITEM_GOLD: d.AddGoldTrade(from, it.data1); break;
	case TRADE_ITEM_GOLD_PER_TURN: d.AddGoldPerTurnTrade(from, it.data1, duration); break;
	case TRADE_ITEM_RESOURCES: d.AddResourceTrade(from, (ResourceTypes)it.data1, it.data2, duration); break;
	case TRADE_ITEM_ALLOW_EMBASSY: d.AddAllowEmbassy(from); break;
	case TRADE_ITEM_OPEN_BORDERS: d.AddOpenBorders(from, duration); break;
	case TRADE_ITEM_DEFENSIVE_PACT: d.AddDefensivePact(from, duration); break;
	case TRADE_ITEM_RESEARCH_AGREEMENT: d.AddResearchAgreement(from, duration); break;
	case TRADE_ITEM_PEACE_TREATY: d.AddPeaceTreaty(from, GC.getPEACE_TREATY_LENGTH()); break;
	case TRADE_ITEM_THIRD_PARTY_PEACE: d.AddThirdPartyPeace(from, (TeamTypes)it.data1, GC.getPEACE_TREATY_LENGTH()); break;
	case TRADE_ITEM_THIRD_PARTY_WAR: d.AddThirdPartyWar(from, (TeamTypes)it.data1); break;
	default: return false;
	}
	return d.GetNumItems() > before;
}

void PushItems(lua_State* L, const CvDeal& d)
{
	lua_newtable(L);
	int index = 0;
	for (TradedItemList::const_iterator it = d.m_TradedItems.begin(); it != d.m_TradedItems.end(); ++it)
	{
		lua_newtable(L);
		IntField(L, "type", it->m_eItemType);
		IntField(L, "from", it->m_eFromPlayer);
		IntField(L, "data1", it->m_iData1);
		IntField(L, "data2", it->m_iData2);
		IntField(L, "data3", it->m_iData3);
		IntField(L, "duration", it->m_iDuration);
		lua_pushboolean(L, it->m_bFlag1);
		lua_setfield(L, -2, "flag1");
		lua_rawseti(L, -2, ++index);
	}
}

std::string DealHex(CvDeal& d)
{
	static const char digits[] = "0123456789abcdef";
	FMemoryStream s;
	std::auto_ptr<ICvDeal1> w = GC.WrapDealPointer(&d);
	w->Write(s);
	const unsigned char* p = (const unsigned char*)s.GetBuf();
	const unsigned int n = s.GetEOF();
	std::string out;
	out.reserve(n * 2);
	for (unsigned int i = 0; i < n; ++i)
	{
		out += digits[p[i] >> 4];
		out += digits[p[i] & 15];
	}
	return out;
}

// One candidate of the trade screen's pockets: a kind of item a side could put on the table now.
struct PocketRow
{
	int from, type, data1, data2, max;
};

// GcUiDealQuery(seat, ai, items, op[, mode]): the AI side of the trade screen for a draft from `seat` to `ai` (the deal of UI.DoProposeDeal).
//  "check":    a list of booleans, one per item: the core takes the item after the ones before it (TradeLogic adds them in that order).
//  "pockets":  what each side could still add: {from, type, data1, data2, max} rows for gold, gold per turn, the tradeable resources,
//              embassy, open borders, defensive pact, research agreement and peace (IsPossibleToTradeItem with the draft on the table).
//  "evaluate": {response = "accept" | "unacceptable" | "insulting", value, mine, theirs, peace}: DoHumanOfferDealToThisAI's verdict
//              (IsDealWithHumanAcceptable, a gift accepted, the "insulting" bounds); value is what sendNetDealAccepted carries.
//  "equalize": {ok, good, cant_match, items}: DoEqualizeDealWithHuman on a copy; mode 0 make it work, 1 what the AI wants (its items stay),
//              2 what the AI would give (the seat's items stay).
//  "blob":     the deal as the hex of ICvDeal1::Write, the text of the seat's sendNetDealAccepted / sendNetDealProposed.
int lDealQuery(lua_State* L)
{
	const PlayerTypes seat = ReadHumanSeat(L, 1);
	const int to = (int)luaL_checkinteger(L, 2);
	if (to < 0 || to >= MAX_MAJOR_CIVS || to == (int)seat || !GET_PLAYER((PlayerTypes)to).isAlive()
		|| !GET_TEAM(GET_PLAYER(seat).getTeam()).isHasMet(GET_PLAYER((PlayerTypes)to).getTeam()))
		return luaL_argerror(L, 2, "expected a living major civilization the seat has met");
	const PlayerTypes other = (PlayerTypes)to;
	std::vector<DraftItem> items;
	ReadDraft(L, 3, seat, other, items);
	const std::string op = luaL_checkstring(L, 4);
	const int mode = (int)luaL_optinteger(L, 5, 0);
	const bool ai = !GET_PLAYER(other).isHuman();
	if ((op == "evaluate" || op == "equalize") && !ai) return luaL_argerror(L, 2, "only an AI evaluates a deal");
	if (op != "check" && op != "pockets" && op != "evaluate" && op != "equalize" && op != "blob") return luaL_argerror(L, 4, "unknown deal query");

	// Everything below runs as the seat with no Lua error until the results are pushed.
	std::vector<bool> taken;
	std::vector<PocketRow> pockets;
	CvDeal d(seat, other);
	DealOfferResponseTypes response = NO_DEAL_RESPONSE_TYPE;
	int value = 0, mine = 0, theirs = 0;
	bool acceptable = false, good = false, cant = false, peace = false;
	std::string blob;
	{
		ActiveAs as(seat);
#if defined(LEKMOD_HEADLESS_FIX_0023) || defined(LEKMOD_HEADLESS_FIX_0030)
		HEADLESS_UI_TREASURY_SCOPE();
#endif
		for (size_t i = 0; i < items.size(); ++i) taken.push_back(AddDraftItem(d, items[i]));
		if (op == "pockets")
		{
			const PlayerTypes sides[2] = {seat, other};
			const int duration = GC.getGame().GetDealDuration();
			for (int s = 0; s < 2; ++s)
			{
				const PlayerTypes from = sides[s], dest = sides[1 - s];
				CvPlayer& p = GET_PLAYER(from);
				PocketRow r;
				r.from = from;
				r.data1 = r.data2 = 0;
				r.type = TRADE_ITEM_GOLD;
				r.max = d.GetGoldAvailable(from, TRADE_ITEM_GOLD);
				if (r.max > 0 && d.IsPossibleToTradeItem(from, dest, TRADE_ITEM_GOLD, 1)) pockets.push_back(r);
				r.type = TRADE_ITEM_GOLD_PER_TURN;
				r.max = p.calculateGoldRate();
				for (TradedItemList::iterator it = d.m_TradedItems.begin(); it != d.m_TradedItems.end(); ++it)
					if (it->m_eItemType == TRADE_ITEM_GOLD_PER_TURN && it->m_eFromPlayer == from) r.max -= it->m_iData1;
				if (r.max > 0 && d.IsPossibleToTradeItem(from, dest, TRADE_ITEM_GOLD_PER_TURN, 1, duration)) pockets.push_back(r);
				for (int res = 0; res < GC.getNumResourceInfos(); ++res)
				{
					const CvResourceInfo* info = GC.getResourceInfo((ResourceTypes)res);
					if (!info || info->getResourceUsage() == RESOURCEUSAGE_BONUS) continue;
					const int have = p.getNumResourceAvailable((ResourceTypes)res, false);
					if (have <= 0) continue;
					// TradeLogic offers one copy of a luxury and every available copy of a strategic resource.
					if (!d.IsPossibleToTradeItem(from, dest, TRADE_ITEM_RESOURCES, res, 1)) continue;
					r.type = TRADE_ITEM_RESOURCES;
					r.data1 = res;
					r.data2 = 0;
					r.max = info->getResourceUsage() == RESOURCEUSAGE_LUXURY ? 1 : have;
					pockets.push_back(r);
				}
				r.data1 = r.data2 = 0;
				r.max = 1;
				const TradeableItems singles[] = {TRADE_ITEM_ALLOW_EMBASSY, TRADE_ITEM_OPEN_BORDERS, TRADE_ITEM_DEFENSIVE_PACT, TRADE_ITEM_RESEARCH_AGREEMENT, TRADE_ITEM_PEACE_TREATY};
				for (size_t k = 0; k < sizeof(singles) / sizeof(singles[0]); ++k)
				{
					const TradeableItems t = singles[k];
					const bool possible = (t == TRADE_ITEM_ALLOW_EMBASSY || t == TRADE_ITEM_PEACE_TREATY) ? d.IsPossibleToTradeItem(from, dest, t)
						: d.IsPossibleToTradeItem(from, dest, t, duration);
					if (!possible) continue;
					r.type = t;
					pockets.push_back(r);
				}
			}
		}
		else if (op == "evaluate")
		{
			int over = 0, under = 0;
			CvDealAI* dealAI = GET_PLAYER(other).GetDealAI();
			peace = d.IsPeaceTreatyTrade(seat);
			acceptable = d.GetNumItems() > 0 && dealAI->IsDealWithHumanAcceptable(&d, seat, value, mine, theirs, over, under, cant);
			// DoHumanOfferDealToThisAI: more than we are asked for (a gift) is accepted too.
			if (!acceptable && d.GetNumItems() > 0 && !peace && theirs > mine) acceptable = true;
			if (acceptable)
			{
				response = DEAL_RESPONSE_ACCEPTABLE;
#if defined(LEKMOD_HEADLESS_FIX_0026)
				value -= dealAI->GetCachedValueOfPeaceWithHuman();
#endif
			}
			else if (value > -75 && mine < theirs * 5)
				response = DEAL_RESPONSE_UNACCEPTABLE;
			else
				response = DEAL_RESPONSE_INSULTING;
		}
		else if (op == "equalize")
		{
			good = false;
			acceptable = GET_PLAYER(other).GetDealAI()->DoEqualizeDealWithHuman(&d, seat, mode == 1, mode == 2, good, cant);
		}
		else if (op == "blob")
			blob = DealHex(d);
	}

	if (op == "check")
	{
		lua_newtable(L);
		for (size_t i = 0; i < taken.size(); ++i)
		{
			lua_pushboolean(L, taken[i]);
			lua_rawseti(L, -2, (int)i + 1);
		}
		return 1;
	}
	if (op == "pockets")
	{
		lua_newtable(L);
		for (size_t i = 0; i < pockets.size(); ++i)
		{
			lua_newtable(L);
			IntField(L, "from", pockets[i].from);
			IntField(L, "type", pockets[i].type);
			IntField(L, "data1", pockets[i].data1);
			IntField(L, "data2", pockets[i].data2);
			IntField(L, "max", pockets[i].max);
			lua_rawseti(L, -2, (int)i + 1);
		}
		return 1;
	}
	if (op == "blob")
	{
		for (size_t i = 0; i < taken.size(); ++i)
			if (!taken[i]) return luaL_error(L, "the deal holds an item the core does not take");
		lua_pushstring(L, blob.c_str());
		return 1;
	}
	lua_newtable(L);
	if (op == "evaluate")
	{
		lua_pushstring(L, response == DEAL_RESPONSE_ACCEPTABLE ? "accept" : (response == DEAL_RESPONSE_UNACCEPTABLE ? "unacceptable" : "insulting"));
		lua_setfield(L, -2, "response");
		IntField(L, "value", value);
		IntField(L, "mine", mine);
		IntField(L, "theirs", theirs);
		lua_pushboolean(L, peace);
		lua_setfield(L, -2, "peace");
		lua_pushboolean(L, cant);
		lua_setfield(L, -2, "cant_match");
		return 1;
	}
	lua_pushboolean(L, acceptable);
	lua_setfield(L, -2, "ok");
	lua_pushboolean(L, good);
	lua_setfield(L, -2, "good");
	lua_pushboolean(L, cant);
	lua_setfield(L, -2, "cant_match");
	PushItems(L, d);
	lua_setfield(L, -2, "items");
	return 1;
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

int RtUiStartDiploRepeat() { return g_startDiploRepeat; }

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
		{"GcUiLeaderGreeting", lLeaderGreeting},
		{"GcUiDiploText", lDiploText},
		{"GcUiDealQuery", lDealQuery},
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
