// Scenario `diplo` (B4 fix-deal-crash; run.sh --scenario diplo [--humans 1] [--ai 1] [--mode hybrid|simultaneous]): the diplomacy of a human with an AI the way the trade and
// discussion screens send it, every message through the server's door (gc_net_submit of the human's seat, delivered by the next gc_pump), on the server (no UI, no replica):
//  - at peace: the discussion events (wants discussion, don't settle, work with us, end work with us; trade screen closed with FIX-0027), deals of the trade screen accepted by the AI
//    (sendNetDealAccepted: embassies both ways, open borders both ways, a research agreement), a demand the AI gave in to (FROM_UI_DIPLO_EVENT_HUMAN_DEMAND + sendNetDemandAccepted);
//  - declare war (sendChangeWar + FROM_UI_DIPLO_EVENT_HUMAN_DECLARES_WAR);
//  - negotiate peace (FROM_UI_DIPLO_EVENT_HUMAN_NEGOTIATE_PEACE, LeaderHeadRoot's trade button at war) while the AI does not want peace, then while it does: the AI opens the trade
//    screen (CvDealAI::DoTradeScreenOpened) and writes the peace items into the UI's scratch deal. The server has no UI: GetScratchDeal was NULL there and the worker died on
//    CvDeal::ClearItems (live game 925, 2026-10-30, "proposed peace to the AI through the trade screen"); now the server keeps a deal of its own;
//  - the peace deal accepted (sendNetDealAccepted with the two peace treaties): no war, a forced peace; then a turn ends and the game goes on.
// The game state the screens need is set by the scenario (meeting, cities, techs, gold, friendship, the AI's wish for peace); the messages themselves are the UI's.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "CvDealClasses.h"
#include "CvDiplomacyAI.h"
#include "CvNotifications.h"
#include "CvTechClasses.h"
#include "../abi/gc_internal.h"
#include "../abi/gc_replica.h"
#include "rt_active.h"
#include "rt_selftest.h"
#include "rt_selftest_slots.h"
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
		printf("diplo: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
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

bool PumpToHumans(gc_instance* inst, gc_pump_result* pr, std::string* why)
{
	gc_error err;
	memset(&err, 0, sizeof(err));
	for (int calls = 0; calls < 20000; ++calls)
	{
		const int32_t rc = gc_pump(inst, 64, pr, &err);
		if (rc != GC_OK) { *why = Format("gc_pump: %d %s", rc, err.message); return false; }
		if (pr->status == GC_PUMP_WAITING_HUMAN) return true;
		if (pr->status == GC_PUMP_GAME_OVER || pr->status == GC_PUMP_STUCK) { *why = Format("gc_pump status %d at turn %d", pr->status, pr->turn); return false; }
	}
	*why = "no WAITING_HUMAN after 20000 gc_pump calls";
	return false;
}

// The hex of ICvDeal1::Write, as RtEngine::sendNetDealAccepted puts it into the message (rt_net.cpp DealToHex).
std::string DealHex(CvDeal& deal)
{
	std::auto_ptr<ICvDeal1> d = GC.WrapDealPointer(&deal);
	FMemoryStream s;
	d->Write(s);
	const unsigned char* p = (const unsigned char*)s.GetBuf();
	static const char kHex[] = "0123456789abcdef";
	std::string out;
	for (unsigned int i = 0; i < s.GetEOF(); ++i)
	{
		out += kHex[p[i] >> 4];
		out += kHex[p[i] & 15];
	}
	return out;
}

RtNetMessage Net(int id, int a0 = 0, int a1 = 0, int a2 = 0, int a3 = 0)
{
	RtNetMessage m;
	m.eId = id;
	m.aiArg[0] = a0; m.aiArg[1] = a1; m.aiArg[2] = a2; m.aiArg[3] = a3;
	return m;
}

RtNetMessage DiploEvent(int ai, int ev, int a1 = 0, int a2 = 0) { return Net(kRtNet_sendFromUIDiploEvent, ai, ev, a1, a2); }

RtNetMessage DealMsg(int id, int to, CvDeal& deal)
{
	RtNetMessage m = Net(id, to);
	m.strText = DealHex(deal);
	return m;
}

// The human's UI sends `msgs` (one GCN1 frame); then single-slice gc_pump calls deliver them. rc of gc_net_submit.
int32_t Send(gc_instance* inst, int player, const std::vector<RtNetMessage>& msgs, gc_pump_result* pr, std::string* note)
{
	std::vector<uint8_t> bytes;
	EncodeNet(msgs, bytes);
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t rc = gc_net_submit(inst, player, bytes.empty() ? NULL : &bytes[0], (uint32_t)bytes.size(), &err);
	if (rc != GC_OK && note) *note = Format("gc_net_submit: %d %s", rc, err.message);
	for (int i = 0; i < 4; ++i) gc_pump(inst, 1, pr, &err);
	return rc;
}

int32_t Send1(gc_instance* inst, int player, const RtNetMessage& m, gc_pump_result* pr, std::string* note)
{
	std::vector<RtNetMessage> v(1, m);
	return Send(inst, player, v, pr, note);
}

void GiveTechs(TeamTypes team, PlayerTypes who)
{
	for (int i = 0; i < GC.getNumTechInfos(); ++i)
	{
		const CvTechEntry* t = GC.getTechInfo((TechTypes)i);
		if (!t) continue;
		if (t->IsAllowEmbassyTradingAllowed() || t->IsOpenBordersTradingAllowed() || t->IsResearchAgreementTradingAllowed())
			if (!GET_TEAM(team).GetTeamTechs()->HasTech((TechTypes)i)) GET_TEAM(team).setHasTech((TechTypes)i, true, who, false, false);
	}
}

bool FoundCity(PlayerTypes p)
{
	CvPlayer& pl = GET_PLAYER(p);
	if (pl.getNumCities() > 0) return true;
	int iLoop = 0;
	for (CvUnit* u = pl.firstUnit(&iLoop); u; u = pl.nextUnit(&iLoop))
		if (u->isFound() && u->canFound(u->plot()))
		{
			u->found();
			break;
		}
	return pl.getNumCities() > 0;
}

int PeaceItems(CvDeal* d)
{
	int n = 0;
	if (d)
		for (TradedItemList::iterator it = d->m_TradedItems.begin(); it != d->m_TradedItems.end(); ++it)
			if (it->m_eItemType == TRADE_ITEM_PEACE_TREATY) ++n;
	return n;
}

std::string Num(long long v) { return Format("%lld", v); }

// The player's notifications whose summary is "Deal Proposed" (TXT_KEY_DEAL_OFFERED: NOTIFICATION_PLAYER_DEAL / _RECEIVED of a proposal between humans).
int OfferedNotes(PlayerTypes p)
{
	CvNotifications* n = GET_PLAYER(p).GetNotifications();
	if (!n) return 0;
	const std::string want = Localization::Lookup("TXT_KEY_DEAL_OFFERED").toUTF8();
	int count = 0;
	for (int i = 0; i < n->GetNumNotifications(); ++i)
		if (std::string(n->GetNotificationSummary(i).c_str()) == want) ++count;
	return count;
}

}  // namespace

extern "C" int rt_selftest_diplo(const gc_host* host, unsigned seed, unsigned turns, unsigned humans, unsigned ai, const char* mode, const char* world_size, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	if (humans < 1) humans = 1;
	if (humans > 4) humans = 4;
	if (ai < 1) ai = 1;
	const std::string turnMode = mode && *mode ? mode : "hybrid";
	(void)turns;

	gc_instance* inst = gc_create(&mine, &err);
	c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);

	std::string stopReason;
	int startTurn = -1, endTurn = -1;
	if (inst)
	{
		std::string players;
		for (unsigned i = 0; i < humans; ++i)
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"human%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
		for (unsigned i = 0; i < ai; ++i) players += ",{\"kind\":\"ai\",\"civ\":\"random\"}";
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"%s\",\"handicap\":\"HANDICAP_PRINCE\"%s%s%s}", seed, players.c_str(), turnMode.c_str(),
			world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		const int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s", setup.c_str(), rc, err.message);
		RtSession* session = InstanceSession(inst);
		if (rc == GC_OK && session)
		{
			int A = -1, X = -1;
			for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
			{
				if (A < 0 && IsSeatedHuman(i)) A = i;
				if (X < 0 && GET_PLAYER((PlayerTypes)i).isAlive() && !IsSeatedHuman(i) && (int)ServerSeat() != i) X = i;
			}
			gc_pump_result pr;
			memset(&pr, 0, sizeof(pr));
			startTurn = GC.getGame().getGameTurn();
			if (A < 0 || X < 0 || !PumpToHumans(inst, &pr, &stopReason))
			{
				if (stopReason.empty()) stopReason = "the game has no human and AI";
			}
			else
			{
				const PlayerTypes pA = (PlayerTypes)A, pX = (PlayerTypes)X;
				const TeamTypes tA = GET_PLAYER(pA).getTeam(), tX = GET_PLAYER(pX).getTeam();
				CvDiplomacyAI* aiDiplo = GET_PLAYER(pX).GetDiplomacyAI();
				// ---- scenario-only state: met, a city each, the techs of embassies / open borders / research agreements, gold, friendship ----
				if (!GET_TEAM(tA).isHasMet(tX)) GET_TEAM(tA).meet(tX, true);
				const bool citiesA = FoundCity(pA), citiesX = FoundCity(pX);
				GiveTechs(tA, pA);
				GiveTechs(tX, pX);
				GET_PLAYER(pA).GetTreasury()->ChangeGold(2000);
				GET_PLAYER(pX).GetTreasury()->ChangeGold(2000);
				aiDiplo->SetDoFAccepted(pA, true);
				GET_PLAYER(pA).GetDiplomacyAI()->SetDoFAccepted(pX, true);
				c.add("setup", GET_TEAM(tA).isHasMet(tX) && citiesA && citiesX && GET_TEAM(tA).isAllowEmbassyTradingAllowed() && GET_TEAM(tX).isAllowEmbassyTradingAllowed(),
					"human %d team %d, AI %d team %d, met %d, cities %d/%d, embassy tech %d/%d", A, (int)tA, X, (int)tX, GET_TEAM(tA).isHasMet(tX) ? 1 : 0, GET_PLAYER(pA).getNumCities(),
					GET_PLAYER(pX).getNumCities(), GET_TEAM(tA).isAllowEmbassyTradingAllowed() ? 1 : 0, GET_TEAM(tX).isAllowEmbassyTradingAllowed() ? 1 : 0);

				// ---- the discussion screen at peace ----
				{
					std::string note;
					const int evs[] = {FROM_UI_DIPLO_EVENT_HUMAN_WANTS_DISCUSSION, FROM_UI_DIPLO_EVENT_HUMAN_DISCUSSION_DONT_SETTLE, FROM_UI_DIPLO_EVENT_HUMAN_DISCUSSION_WORK_WITH_US,
						FROM_UI_DIPLO_EVENT_HUMAN_DISCUSSION_END_WORK_WITH_US
#if defined(LEKMOD_HEADLESS_FIX_0027)
						, HEADLESS_FROM_UI_DIPLO_EVENT_TRADE_SCREEN_CLOSED
#endif
					};
					int ok = 0;
					for (size_t i = 0; i < sizeof(evs) / sizeof(evs[0]); ++i)
						if (Send1(inst, A, DiploEvent(X, evs[i]), &pr, &note) == GC_OK) ++ok;
					c.add("discussion", ok == (int)(sizeof(evs) / sizeof(evs[0])), "%d of %d discussion events taken%s%s", ok, (int)(sizeof(evs) / sizeof(evs[0])), note.empty() ? "" : "; ", note.c_str());
				}

				// ---- deals of the trade screen at peace (the AI accepted them on the replica: sendNetDealAccepted) ----
				// The friendship again (END_WORK_WITH_US above ended it): gold and research agreements need it.
				aiDiplo->SetDoFAccepted(pA, true);
				GET_PLAYER(pA).GetDiplomacyAI()->SetDoFAccepted(pX, true);
				{
					std::string note;
					CvDeal emb;
					emb.SetFromPlayer(pA);
					emb.SetToPlayer(pX);
					emb.AddAllowEmbassy(pA);
					emb.AddAllowEmbassy(pX);
					const int items = emb.GetNumItems();
					const int32_t r = Send1(inst, A, DealMsg(kRtNet_sendNetDealAccepted, X, emb), &pr, &note);
					c.add("deal_embassy", items == 2 && r == GC_OK && GET_TEAM(tA).HasEmbassyAtTeam(tX) && GET_TEAM(tX).HasEmbassyAtTeam(tA),
						"%d item(s), submit rc %d%s%s; embassies %d/%d", items, r, note.empty() ? "" : " ", note.c_str(), GET_TEAM(tA).HasEmbassyAtTeam(tX) ? 1 : 0, GET_TEAM(tX).HasEmbassyAtTeam(tA) ? 1 : 0);
				}
				{
					std::string note;
					const int dur = GC.getGame().GetDealDuration();
					CvDeal ob;
					ob.SetFromPlayer(pA);
					ob.SetToPlayer(pX);
					ob.AddOpenBorders(pA, dur);
					ob.AddOpenBorders(pX, dur);
					const int items = ob.GetNumItems();
					const int32_t r = Send1(inst, A, DealMsg(kRtNet_sendNetDealAccepted, X, ob), &pr, &note);
					c.add("deal_open_borders", items == 2 && r == GC_OK && GET_TEAM(tA).IsAllowsOpenBordersToTeam(tX) && GET_TEAM(tX).IsAllowsOpenBordersToTeam(tA),
						"%d item(s), submit rc %d%s%s; open borders %d/%d", items, r, note.empty() ? "" : " ", note.c_str(), GET_TEAM(tA).IsAllowsOpenBordersToTeam(tX) ? 1 : 0,
						GET_TEAM(tX).IsAllowsOpenBordersToTeam(tA) ? 1 : 0);
				}
				{
					std::string note;
					const int dur = GC.getGame().GetDealDuration();
					CvDeal ra;
					ra.SetFromPlayer(pA);
					ra.SetToPlayer(pX);
					ra.AddResearchAgreement(pA, dur);
					ra.AddResearchAgreement(pX, dur);
					const int items = ra.GetNumItems();
					const int32_t r = Send1(inst, A, DealMsg(kRtNet_sendNetDealAccepted, X, ra), &pr, &note);
					// The rules of a research agreement (techs, embassies, friendship, gold) are the game's: an item it refuses is not added; whichever way, the server lives on.
					c.add("deal_research_agreement", r == GC_OK && (items == 0 || GET_TEAM(tA).IsHasResearchAgreement(tX)),
						"%d item(s), submit rc %d%s%s; research agreement %d", items, r, note.empty() ? "" : " ", note.c_str(), GET_TEAM(tA).IsHasResearchAgreement(tX) ? 1 : 0);
				}
				{
					// UI.DoDemand: the AI gave in on the replica (DoHumanDemand): FROM_UI_DIPLO_EVENT_HUMAN_DEMAND with its answer, then the deal as sendNetDemandAccepted.
					std::string note;
					CvDeal dem;
					dem.SetFromPlayer(pA);
					dem.SetToPlayer(pX);
					dem.SetDemandingPlayer(pA);
					dem.AddGoldTrade(pX, 50);
					const int items = dem.GetNumItems();
					const int goldA = GET_PLAYER(pA).GetTreasury()->GetGold();
					std::vector<RtNetMessage> v;
					v.push_back(DiploEvent(X, FROM_UI_DIPLO_EVENT_HUMAN_DEMAND, DEMAND_RESPONSE_ACCEPT, -1));
					v.push_back(DealMsg(kRtNet_sendNetDemandAccepted, X, dem));
					const int32_t r = Send(inst, A, v, &pr, &note);
					const int goldA2 = GET_PLAYER(pA).GetTreasury()->GetGold();
					c.add("demand", items == 1 && r == GC_OK && goldA2 >= goldA + 50, "%d item(s), submit rc %d%s%s; the human's gold %d -> %d", items, r, note.empty() ? "" : " ", note.c_str(), goldA, goldA2);
				}

				// ---- B4 fix-mp-trade: a deal between two humans (--humans 2): proposed by A (UI.DoProposeDeal to a human), accepted by B (UI.DoFinalizePlayerDeal) ----
				int B = -1;
				for (int i = 0; i < MAX_MAJOR_CIVS && humans > 1; ++i)
					if (i != A && IsSeatedHuman(i) && GET_PLAYER((PlayerTypes)i).isAlive()) { B = i; break; }
				if (B >= 0)
				{
					const PlayerTypes pB = (PlayerTypes)B;
					const TeamTypes tB = GET_PLAYER(pB).getTeam();
					if (!GET_TEAM(tA).isHasMet(tB)) GET_TEAM(tA).meet(tB, true);
					GET_PLAYER(pB).GetTreasury()->ChangeGold(500);
					CvGameDeals* deals = GC.getGame().GetGameDeals();
					// A declaration of friendship first: gold between two civilizations needs one (CvDeal::IsPossibleToTradeItem).
					{
						std::string note;
						CvDeal dof;
						dof.SetFromPlayer(pA);
						dof.SetToPlayer(pB);
						dof.AddDeclarationOfFriendship(pA);
						dof.AddDeclarationOfFriendship(pB);
						const int items = dof.GetNumItems();
						Send1(inst, A, DealMsg(kRtNet_sendNetDealProposed, B, dof), &pr, &note);
						const bool stored = deals->ProposedDealExists(pA, pB);
						const int offeredB = OfferedNotes(pB);
						const int32_t r = Send1(inst, B, Net(kRtNet_sendNetPlayerDealFinalized, A, B, 1), &pr, &note);
						const bool dofA = GET_PLAYER(pA).GetDiplomacyAI()->IsDoFAccepted(pB), dofB = GET_PLAYER(pB).GetDiplomacyAI()->IsDoFAccepted(pA);
						c.add("pvp_friendship", items == 2 && stored && offeredB > 0 && r == GC_OK && dofA && dofB && !deals->ProposedDealExists(pA, pB),
							"%d item(s), stored %d, B's 'deal proposed' notifications %d, submit rc %d%s%s; friendship %d/%d", items, stored ? 1 : 0, offeredB, r, note.empty() ? "" : " ",
							note.c_str(), dofA ? 1 : 0, dofB ? 1 : 0);
					}
					CvDeal gift;
					gift.SetFromPlayer(pA);
					gift.SetToPlayer(pB);
					gift.AddGoldTrade(pA, 100);
					const int goldA = GET_PLAYER(pA).GetTreasury()->GetGold(), goldB = GET_PLAYER(pB).GetTreasury()->GetGold();
					std::string note;
					// A forged proposal in B's name (the sender is the connection's seat) and a proposal to oneself are dropped.
					Send1(inst, B, DealMsg(kRtNet_sendNetDealProposed, B, gift), &pr, &note);
					const bool forged = deals->ProposedDealExists(pA, pB) || deals->ProposedDealExists(pB, pB);
					int32_t r = Send1(inst, A, DealMsg(kRtNet_sendNetDealProposed, B, gift), &pr, &note);
					const bool stored = deals->ProposedDealExists(pA, pB) && deals->HasMadeProposal(pA) == pB;
					c.add("pvp_proposed", gift.GetNumItems() == 1 && r == GC_OK && !forged && stored, "%d item(s), submit rc %d%s%s; forged dropped %d, stored %d", gift.GetNumItems(), r,
						note.empty() ? "" : " ", note.c_str(), forged ? 0 : 1, stored ? 1 : 0);
					// The proposer cannot accept its own proposal; the receiver does.
					Send1(inst, A, Net(kRtNet_sendNetPlayerDealFinalized, A, B, 1), &pr, &note);
					const bool selfAccept = !deals->ProposedDealExists(pA, pB) || GET_PLAYER(pA).GetTreasury()->GetGold() != goldA;
					r = Send1(inst, B, Net(kRtNet_sendNetPlayerDealFinalized, A, B, 1), &pr, &note);
					const int goldA1 = GET_PLAYER(pA).GetTreasury()->GetGold(), goldB1 = GET_PLAYER(pB).GetTreasury()->GetGold();
					c.add("pvp_accepted", r == GC_OK && !selfAccept && !deals->ProposedDealExists(pA, pB) && goldA1 == goldA - 100 && goldB1 == goldB + 100,
						"submit rc %d; the proposer's own accept ignored %d; gold A %d -> %d, B %d -> %d", r, selfAccept ? 0 : 1, goldA, goldA1, goldB, goldB1);
					// A new proposal, a counter-offer by B (it replaces A's), withdrawn by B: no gold moves.
					Send1(inst, A, DealMsg(kRtNet_sendNetDealProposed, B, gift), &pr, &note);
					CvDeal counter;
					counter.SetFromPlayer(pB);
					counter.SetToPlayer(pA);
					counter.AddGoldTrade(pA, 200);
					Send1(inst, B, DealMsg(kRtNet_sendNetDealProposed, A, counter), &pr, &note);
					const bool replaced = !deals->ProposedDealExists(pA, pB) && deals->ProposedDealExists(pB, pA);
					Send1(inst, B, Net(kRtNet_sendNetPlayerDealFinalized, B, A, 0), &pr, &note);
					const int goldA2 = GET_PLAYER(pA).GetTreasury()->GetGold(), goldB2 = GET_PLAYER(pB).GetTreasury()->GetGold();
					c.add("pvp_counter_withdrawn", replaced && !deals->ProposedDealExists(pB, pA) && goldA2 == goldA1 && goldB2 == goldB1,
						"counter replaced the proposal %d; withdrawn %d; gold A %d -> %d, B %d -> %d", replaced ? 1 : 0, deals->ProposedDealExists(pB, pA) ? 0 : 1, goldA1, goldA2, goldB1, goldB2);
				}

				// ---- declare war (the war button: sendChangeWar, then the AI's answer event) ----
				{
					std::string note;
					std::vector<RtNetMessage> v;
					v.push_back(Net(kRtNet_sendChangeWar, (int)tX, 1));
					v.push_back(DiploEvent(X, FROM_UI_DIPLO_EVENT_HUMAN_DECLARES_WAR));
					const int32_t r = Send(inst, A, v, &pr, &note);
					c.add("declare_war", r == GC_OK && GET_TEAM(tA).isAtWar(tX) && GET_TEAM(tX).isAtWar(tA), "submit rc %d%s%s; at war %d/%d", r, note.empty() ? "" : " ", note.c_str(),
						GET_TEAM(tA).isAtWar(tX) ? 1 : 0, GET_TEAM(tX).isAtWar(tA) ? 1 : 0);
				}

				// ---- negotiate peace while the AI does not want it: the AI says no ----
				{
					std::string note;
					aiDiplo->SetTreatyWillingToOffer(pA, NO_PEACE_TREATY_TYPE);
					aiDiplo->SetTreatyWillingToAccept(pA, NO_PEACE_TREATY_TYPE);
					const int32_t r = Send1(inst, A, DiploEvent(X, FROM_UI_DIPLO_EVENT_HUMAN_NEGOTIATE_PEACE), &pr, &note);
					c.add("negotiate_peace_refused", r == GC_OK && GET_TEAM(tA).isAtWar(tX), "submit rc %d%s%s; still at war %d", r, note.empty() ? "" : " ", note.c_str(), GET_TEAM(tA).isAtWar(tX) ? 1 : 0);
				}

				// ---- negotiate peace while the AI wants it: CvDealAI::DoTradeScreenOpened on the server (the crash of game 925) ----
				{
					std::string note;
					aiDiplo->SetPlayerNumTurnsAtWar(pA, 10);
					aiDiplo->SetTreatyWillingToOffer(pA, PEACE_TREATY_WHITE_PEACE);
					aiDiplo->SetTreatyWillingToAccept(pA, PEACE_TREATY_WHITE_PEACE);
					const bool willing = aiDiplo->IsWillingToMakePeaceWithHuman(pA);
					const int32_t r = Send1(inst, A, DiploEvent(X, FROM_UI_DIPLO_EVENT_HUMAN_NEGOTIATE_PEACE), &pr, &note);
					std::auto_ptr<ICvDeal1> ui(GC.GetEngineUserInterface()->GetScratchDeal());
					CvDeal* scratch = ui.get() ? GC.UnwrapDealPointer(ui.get()) : NULL;
					const int peace = PeaceItems(scratch);
					c.add("negotiate_peace", willing && r == GC_OK && scratch && peace == 2 && scratch->GetFromPlayer() == pA && scratch->GetToPlayer() == pX,
						"AI willing %d, submit rc %d%s%s; the server's scratch deal %s: %d item(s), %d peace treat%s, from %d to %d", willing ? 1 : 0, r, note.empty() ? "" : " ", note.c_str(),
						scratch ? "present" : "NULL", scratch ? scratch->GetNumItems() : -1, peace, peace == 1 ? "y" : "ies", scratch ? (int)scratch->GetFromPlayer() : -1, scratch ? (int)scratch->GetToPlayer() : -1);
				}

				// ---- the peace deal of the trade screen accepted by the AI ----
				{
					std::string note;
					CvDeal pd;
					pd.SetFromPlayer(pA);
					pd.SetToPlayer(pX);
					pd.AddPeaceTreaty(pA, GC.getGame().getGameSpeedInfo().getPeaceDealDuration());
					pd.AddPeaceTreaty(pX, GC.getGame().getGameSpeedInfo().getPeaceDealDuration());
					pd.SetPeaceTreatyType(PEACE_TREATY_WHITE_PEACE);
					const int items = pd.GetNumItems();
					const int32_t r = Send1(inst, A, DealMsg(kRtNet_sendNetDealAccepted, X, pd), &pr, &note);
					c.add("deal_peace", items == 2 && r == GC_OK && !GET_TEAM(tA).isAtWar(tX) && !GET_TEAM(tX).isAtWar(tA) && GET_TEAM(tA).isForcePeace(tX),
						"%d item(s), submit rc %d%s%s; at war %d/%d, forced peace %d", items, r, note.empty() ? "" : " ", note.c_str(), GET_TEAM(tA).isAtWar(tX) ? 1 : 0,
						GET_TEAM(tX).isAtWar(tA) ? 1 : 0, GET_TEAM(tA).isForcePeace(tX) ? 1 : 0);
				}

				// ---- the game goes on: the human ends the turn, the next human phase comes ----
				{
					const int turn0 = GC.getGame().getGameTurn();
					gc_command endc;
					memset(&endc, 0, sizeof(endc));
					endc.kind = (uint16_t)GC_CMD_END_TURN_TIMEOUT;
					// every seated human ends the turn (the second human of --humans 2 too)
					for (int h = 0; h < MAX_MAJOR_CIVS; ++h)
					{
						if (!IsSeatedHuman(h) || !GET_PLAYER((PlayerTypes)h).isAlive()) continue;
						endc.player = (uint8_t)h;
						gc_apply_command(inst, &endc, &err);
					}
					bool next = false;
					for (int calls = 0; calls < 20000 && stopReason.empty(); ++calls)
					{
						const int32_t prc = gc_pump(inst, 64, &pr, &err);
						if (prc != GC_OK) { stopReason = Format("gc_pump: %d %s", prc, err.message); break; }
						if (pr.status == GC_PUMP_GAME_OVER || pr.status == GC_PUMP_STUCK) { stopReason = Format("gc_pump status %d at turn %d", pr.status, pr.turn); break; }
						if (pr.turn != turn0 && pr.status == GC_PUMP_WAITING_HUMAN) { next = true; break; }
					}
					c.add("next_turn", next, "turn %d -> %d", turn0, GC.getGame().getGameTurn());
				}
			}
			endTurn = GC.getGame().getGameTurn();
		}
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("diplo-event|%s\n", seen.lines[i].c_str());
	c.add("no_stop", stopReason.empty(), "%s", stopReason.empty() ? "no STUCK, no GAME_OVER, no gc_pump error" : stopReason.c_str());
	c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
	c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);

	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"mode\":\"" + turnMode + "\",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"start_turn\":" + Num(startTurn) +
		",\"end_turn\":" + Num(endTurn) + ",\"log_errors\":" + Num(seen.logErrors) + ",\"lua_errors\":" + Num(seen.luaErrors) + ",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("diplo-summary|%s|seed %u|humans %u|ai %u|turns %d -> %d|failed %d\n", turnMode.c_str(), seed, humans, ai, startTurn, endTurn, c.failed);
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
