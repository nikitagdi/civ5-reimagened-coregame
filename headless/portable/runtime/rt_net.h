// rt_net: single-player loopback between the DLL's gDLL->send* commands and ICvNetMessageHandler3::Response*.
// Every command the DLL sends is queued; the runtime delivers the queue before the next ICvGame1::Update() (the exe processes received
// net messages between game-core frames; civhost GUESS: delivery at the next slice, not synchronously inside send*).
// Sender = the active player unless the send* method names a player (as the exe's net messages carry the sending peer's player id).
// Ported from headless/host/src/net/HstLoopback.cpp (single mode: no peers, no exe turn handshake, no deal messages; the deal and
// leader-message paths belong to task F1).
#pragma once

#include <string>

#include "CvGameCoreDLLPCH.h"

namespace lekmod_portable {

enum RtNetId
{
	kRtNet_None = 0,
	kRtNet_sendTurnComplete,
	kRtNet_sendTurnUnready,
	kRtNet_SendAICivsProcessed,
	kRtNet_IdeologyChoice,
	kRtNet_ArchaeologyChoice,
	kRtNet_SendDiploVote,
	kRtNet_SendEnhanceReligion,
	kRtNet_SendFaithPurchase,
	kRtNet_SendFoundPantheon,
	kRtNet_SendFoundReligion,
	kRtNet_SendLeagueEditName,
	kRtNet_SendLeagueProposeEnact,
	kRtNet_SendLeagueProposeRepeal,
	kRtNet_SendLeagueVoteAbstain,
	kRtNet_SendLeagueVoteEnact,
	kRtNet_SendLeagueVoteRepeal,
	kRtNet_SendLiberateMinor,
	kRtNet_SendMinorNoUnitSpawning,
	kRtNet_SendMoveSpy,
	kRtNet_SendPledgeMinorProtection,
	kRtNet_SendRenameCity,
	kRtNet_SendRenameUnit,
	kRtNet_SendSellBuilding,
	kRtNet_SendStageCoup,
	kRtNet_SendUpdateCityCitizens,
	kRtNet_sendAdvancedStartAction,
	kRtNet_sendAutoMission,
	kRtNet_sendBarbarianRansom,
	kRtNet_sendChangeIdeology,
	kRtNet_sendChangeWar,
	kRtNet_sendCityBuyPlot,
	kRtNet_sendDoCommand,
	kRtNet_sendDoTask,
	kRtNet_sendFaithGreatPersonChoice,
	kRtNet_sendFromUIDiploEvent,
	kRtNet_sendGiftUnit,
	kRtNet_sendGoodyChoice,
	kRtNet_sendGreatPersonChoice,
	kRtNet_sendIgnoreWarning,
	kRtNet_sendLaunch,
	kRtNet_sendMayaBonusChoice,
	kRtNet_sendMinorBullyGold,
	kRtNet_sendMinorBullyUnit,
	kRtNet_sendMinorBuyout,
	kRtNet_sendMinorCivEnterTerritory,
	kRtNet_sendMinorCivIntrusion,
	kRtNet_sendMinorCivQuestCompleted,
	kRtNet_sendMinorCivQuestNoInterest,
	kRtNet_sendMinorGiftGold,
	kRtNet_sendMinorGiftTileImprovement,
	kRtNet_sendMinorPledgeProtection,
	kRtNet_sendMoveGreatWorks,
	kRtNet_sendPlayerHurry,
	kRtNet_sendPopOrder,
	kRtNet_sendPurchase_1,
	kRtNet_sendPurchase_2,
	kRtNet_sendPushMission,
	kRtNet_sendPushOrder,
	kRtNet_sendResearch,
	kRtNet_sendReturnCivilian,
	kRtNet_sendSetCityAIFocus,
	kRtNet_sendSetCityAvoidGrowth,
	kRtNet_sendSetSwappableGreatWork,
	kRtNet_sendSwapGreatWorks,
	kRtNet_sendSwapOrder,
	kRtNet_sendSwapUnits,
	kRtNet_sendUpdatePolicies,
	kRtNet_sendAutoMoves,
	kRtNet_sendNetDealAccepted,    // B4 M4 WO-D: aiArg[0] = to player, [1..3] = the three deal values, strText = the deal (hex of ICvDeal1::Write)
	kRtNet_sendNetDemandAccepted,  // aiArg[0] = to player, strText = the deal
	// B4 fix-mp-trade: deals between two humans (the exe's NetPlayerDealProposed / NetPlayerDealFinalized).
	kRtNet_sendNetDealProposed,         // aiArg[0] = to player, strText = the deal (hex of ICvDeal1::Write); the sender proposes
	kRtNet_sendNetPlayerDealFinalized,  // aiArg[0] = from player, [1] = to player, [2] = accepted; the sender acts (ResponsePlayerDealFinalized)
	kRtNet_Count
};

struct RtNetMessage
{
	int eId;
	PlayerTypes ePlayer;
	int aiArg[9];
	std::string strText;
	RtNetMessage() : eId(kRtNet_None), ePlayer(NO_PLAYER) { for (int i = 0; i < 9; ++i) aiArg[i] = 0; }
};

}  // namespace lekmod_portable
