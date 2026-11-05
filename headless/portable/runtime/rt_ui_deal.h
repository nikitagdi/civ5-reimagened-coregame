// rt_ui_deal: the trade-screen half of the exe's UI that a replica serves (B4 M4 WO-D, K12). The exe's `UI.GetScratchDeal / DoProposeDeal / DoDemand /
// DoEqualizeDealWithHuman / DoWhatDoesAIWant / DoWhatWillAIGive / ProposedDealExists / LoadProposedDeal / OnHumanOpenedTradeScreen` live here as globals
// of the UI's Lua state (`GcUi*`, registered by gc_lua_attach_ui); the client's exe API wraps them as `UI.*`. They call the same DLL entries as the exe
// (CvDealAI::DoHumanOfferDealToThisAI ...); an accepted deal leaves through gDLL->sendNetDealAccepted -> the outbox -> the server (kRtNet_sendNetDealAccepted).
#pragma once

#include "CvGameCoreDLLPCH.h"

struct lua_State;

namespace lekmod_portable {

// The scratch deal of the seat's UI (what the DLL reads through GetScratchDeal): one object for the life of the process.
CvDeal* RtUiScratchDeal();

class RtEngine;

// Registers the GcUi* globals in L; `libs` pushes Deal instances (the DLL's script system utility); `engine` sends the deals between two humans
// (B4 fix-mp-trade: GcUiProposeDeal to a human, GcUiFinalizePlayerDeal, GcUiHasMadeProposal).
void RegisterUiDealLib(lua_State* L, ICvScriptSystemUtility1* libs, RtEngine* engine);

}  // namespace lekmod_portable
