"""Hand-written part of the command dictionary (v1).

gen.py joins this table with the mechanical parse of the DLL headers (names, argument lists, wire names, source
lines) and with headless/improvements/fixes.json (FIX-VALIDATION flags and status).  Everything here is a decision
or a rule text written by hand after reading the handlers in CvDllNetMessageHandler.cpp (v34.15 + headless fixes):

  C(name, class, send=, resp=, lua=, ...)  one dictionary entry
    app      applicability rule(s)   -> applicability/hand
    rules    full rule strings 'step/hand: text [gap:ID]'
    gaps     ids from GAPS that apply to the entry
    phase    turn_phase tokens: turn_active | any | pending:<ENDTURN_BLOCKING_*|name>
    over     {c++ param name: {arg overrides}}          drop: params that are fixed / derived, not client input
    mux      fixed wire values that select this entry on a multiplexed message
    derived  {c++ param: how the host fills it}

Entries without app/rules get class-default 'auto' rules only (marked auto in the dictionary).
"""
from __future__ import annotations

CMDS: list[dict] = []


def C(name: str, cls: str, **kw) -> None:
    kw.update(name=name, cls=cls)
    CMDS.append(kw)


MAJOR = {"pkind": "major"}
MINOR = {"pkind": "minor"}
BOT = "bot"  # entries the V1 bot-player uses

# ---------------------------------------------------------------------------------------------------------------
# GAPS: validation holes of the Response* handlers.  FIX-01xx rows take flag/status from fixes.json at generation
# time; AUD-xx rows were found while auditing the other handlers (host-validator only, candidates for FIX-VALIDATION).
# ---------------------------------------------------------------------------------------------------------------
GAPS: dict[str, str] = {
    "FIX-0101": "ResponseResearch: iDiscover > 0 without GetNumFreeTechs() > 0 sets NumFreeTechs to iDiscover-1 (arbitrary free techs); the steal branch indexes m_aiNumTechsToStealList[player] / HasTech(tech) without range checks.",
    "FIX-0102": "ResponseChangeWar: team id not range-checked (GET_TEAM out of bounds); war/peace with the own team was only a FAssert; no canDeclareWar/canChangeWarPeace.",
    "FIX-0103": "ResponseBarbarianRansom: nothing checks that a ransom was offered; option 1 kills any own unit by id, option 0 takes gold.",
    "FIX-0104": "ResponseFoundReligion: plot(x,y)->getPlotCity() dereferences NULL off the map (crash); the holy city need not belong to the founder.",
    "FIX-0105": "ResponseEnhanceReligion: the failure branch dereferences plot(x,y) (NULL off the map); player id out of range indexes GET_PLAYER.",
    "FIX-0106": "ResponseDiplomacyFromUI: GET_PLAYER(eOtherPlayer) without a range check.",
    "FIX-0107": "ResponseMinorCiv*/PledgeMinorProtection/MinorNoUnitSpawning: sender and city-state ids unchecked (GetMinorCivAI of a non-minor, out-of-range index).",
    "FIX-0108": "ResponseGoodyChoice: no pending-choice check (any goody at any time) and NULL plot dereference.",
    "FIX-0109": "ResponseLeague*: CanVote/CanProposeEnact/CanProposeRepeal only in CvAssertMsg (compiled out), NULL league, negative vote counts add votes.",
    "FIX-0110": "ResponseUpdatePolicies(bValue=false): setHasPolicy(false) on any policy id, unchecked.",
    "AUD-01": "ResponseMinorCivGiftGold: iGold < 0 passes 'GetGold() >= iGold' and DoGoldGiftFromMajor does ChangeGold(-iGold), so a negative gift credits the sender.",
    "AUD-02": "ResponseFaithGreatPersonChoice: no GetNumFaithGreatPeople() > 0 check (compare ResponseGreatPersonChoice under NET_FIX_SINGLE_USE_ABILITY_DUPE): any number of faith great people.",
    "AUD-03": "ResponseGreatPersonChoice / ResponseMayaBonusChoice: the spawn is gated by the counter (NET_FIX_SINGLE_USE_ABILITY_DUPE) but ChangeNumFreeGreatPeople(-1) / ChangeNumMayaBoosts(-1) run unconditionally: a bogus choice drives the counter negative.",
    "AUD-04": "ResponseIdeologyChoice: SetPolicyBranchUnlocked(choice, true) with only CvAssertMsg range checks and no pending-choice check.",
    "AUD-05": "ResponseDiploVote: GET_PLAYER(eVotePlayer).getTeam() without a range check.",
    "AUD-06": "ResponseGiftUnit: GET_PLAYER(eMinor) without a range check on the -3 (CC proposal) and distance-gift paths.",
    "AUD-07": "ResponsePlayerOption: setOption appends a new serialized entry for every unknown option id (unbounded state growth).",
    "AUD-08": "ResponseDiplomacyFromUI: event id only CvAssertMsg-checked; FROM_UI_DIPLO_EVENT_HUMAN_DECLARES_WAR calls declareWar without canDeclareWar (a second war route next to ResponseChangeWar).",
    "AUD-09": "ResponsePlayerDealFinalized: from/to/actor ids unchecked (GET_PLAYER out of bounds, forged actor).",
    "AUD-10": "ResponseChangeIdeology: only isAlive is checked; DoSwitchIdeologies asserts 'Illegal time for Ideology change' but SetAnarchyNumTurns already ran.",
    "AUD-11": "ResponseSetCityAIFocus: focus id only FAssert-checked in CvCityCitizens::SetFocusType.",
    "AUD-13": "ResponseMinorCivBullyGold: iGold comes from the message and is credited as is (CvAssertMsg only); honest senders send CvMinorCivAI::GetBullyGoldAmount (CvGame::DoMinorBullyGold), so a modified client takes any amount whenever bullying is allowed.",
    "AUD-14": "ResponseMinorCivBullyUnit: the unit type comes from the message (honest sender: always UNIT_WORKER, CvGame::DoMinorBullyUnit) and is not validated: any unit type is spawned for the bully, an out-of-range id dereferences a NULL unit info.",
    "AUD-15": "ResponseReturnCivilian: neither 'the unit is a captured civilian awaiting the decision' nor 'to_player is its previous owner' is checked: any own unit is killed and re-created for any player (a free gift), and a return to a city-state grants RETURN_CIVILIAN_FRIENDSHIP influence every time; eToPlayer is also an unchecked GET_PLAYER index.",
    "AUD-16": "ResponseLiberatePlayer: CvPlayer::DoLiberatePlayer never calls CanLiberatePlayer/CanLiberatePlayerCity (CvAssert on the city only): any own city can be handed to any player, a dead player is resurrected.",
    "AUD-17": "ResponseMoveGreatWorks: CvGameCulture::MoveGreatWorks checks only player/city lookups; building class, slot index and slot type compatibility are not validated (it swaps whatever the two slots hold).",
    "AUD-18": "Client-supplied player id used as a GET_PLAYER index with only a NO_PLAYER/-1 check: ResponseMoveSpy (iTargetPlayer), CvGameCulture::SwapGreatWorks (ePlayer2), CvPlayer::DoLiberatePlayer (ePlayer).",
    "AUD-19": "ResponseCityDoTask -> CvCity::doTask: TASK_GIFT hands any own city to any player id (iData1 unchecked, no can* check) and TASK_DISBAND deletes any own city; no shipped sender uses either. (RAZE, HURRY, CONSCRIPT and RANGED_ATTACK re-check canRaze/canHurry/canConscript/canRangeStrikeAt inside.)",
    "AUD-12": "CvCity::pushOrder clears the order queue (replace flag) before canTrain/canConstruct/canCreate: an invalid order with the replace flag empties the queue.",
}

# ---------------------------------------------------------------------------------------------------------------
# unit.*
# ---------------------------------------------------------------------------------------------------------------
C("unit.push_mission", "player", send="sendPushMission", resp="ResponsePushMission", rate="unit", tags=[BOT],
  over={"iUnitID": {}, "eMission": {}, "iData1": {"name": "data1", "note": "plot x | target unit id | build id, by mission"},
        "iData2": {"name": "data2", "note": "plot y | extra, by mission"},
        "iFlags": {"name": "flags", "type": "bitset:MoveFlags"}, "bShift": {"name": "append"}},
  rules=["ownership/hand: unit exists and owner == session.player (handler: GET_PLAYER(ePlayer).getUnit(id) only)",
         "range/hand: mission is a Missions Type (the handler remaps MISSION_PARADROP to -2 under REMOVE_PARADROP_ANIMATION); plot (data1, data2) inside map bounds for plot missions",
         "applicability/hand: CvUnitMission::CanStartMission(unit, mission, data1, data2, plot(data1,data2)) (runs the Lua hook CanStartMission, so the host query must be hook-neutral, OQ 4). The DLL re-checks it when the mission starts (CvUnitMission::ContinueMission) but queues anything",
         "applicability/hand: move missions: CvUnit::canMoveInto / canMoveOrAttackInto on the target plot"],
  note="Settlers: MISSION_MOVE_TO then MISSION_FOUND (helpers unit.move_to, unit.found_city).")
C("unit.swap_units", "player", send="sendSwapUnits", resp="ResponseSwapUnits", rate="unit",
  drop=["eMission", "iFlags"], derived={"eMission": "MISSION_MOVE_TO (ignored by the handler)", "iFlags": "0 (ignored by the handler)"},
  over={"iData1": {"name": "x"}, "iData2": {"name": "y"}, "bShift": {"name": "append"}},
  app="target plot must exist (handler checks) and hold a unit of the same type (handler loops AreUnitsOfSameType); the first push uses MOVE_IGNORE_STACKING; neither push is gated by canMoveInto here")
C("unit.do_command", "player", send="sendDoCommand", resp="ResponseDoCommand", rate="unit",
  over={"iData1": {"name": "data1"}, "iData2": {"name": "data2"}, "bAlt": {"name": "all_of_type", "note": "apply to every own unit of the same type when the command has getAll"}},
  rules=["applicability/hand: CvUnit::canDoCommand(command, data1, data2); CvUnit::doCommand re-checks it. Handler: command info is NULL-checked only under AUI_WARNING_FIXES (enabled)"])
C("unit.automate", "player", send="sendDoCommand", resp="ResponseDoCommand", rate="unit", tags=[BOT],
  mux={"eCommand": "COMMAND_AUTOMATE"}, drop=["eCommand", "iData2"], derived={"eCommand": "COMMAND_AUTOMATE", "iData2": 0},
  over={"iData1": {"name": "mode", "type": "enum:Automates", "note": "AUTOMATE_BUILD | AUTOMATE_EXPLORE | AUTOMATE_TRADE"}, "bAlt": {"name": "all_of_type"}},
  rules=["applicability/hand: CvUnit::CanAutomate(mode) via canDoCommand(COMMAND_AUTOMATE, mode); workers use AUTOMATE_BUILD, scouts AUTOMATE_EXPLORE (bot decision table)"])
C("unit.auto_mission", "player", send="sendAutoMission", resp="ResponseAutoMission", rate="unit",
  app="handler only looks the unit up and calls CvUnit::AutoMission()")
C("unit.destroy", "player", resp="ResponseDestroyUnit", rate="unit",
  note="No send* in ICvEngineUtility and no Network.Send* in the shipped Lua: sent by the engine (unit disband action).",
  app="handler looks the unit up and kills it (disband); no confirmation state; own unit only, so the exposure is self-harm")
C("unit.rename", "player", lua="SendRenameUnit", send="SendRenameUnit", resp="ResponseRenameUnit", rate="rename",
  app="handler looks the unit up; the name is free text (host limits length and characters)")

# ---------------------------------------------------------------------------------------------------------------
# city.*
# ---------------------------------------------------------------------------------------------------------------
C("city.push_order", "player", send="sendPushOrder", resp="ResponseCityPushOrder", rate="city", tags=[BOT], gaps=["AUD-12"],
  over={"eOrder": {"name": "order"},
        "iData": {"name": "data", "typed_by": {"arg": "order", "map": {"ORDER_TRAIN": "enum:Units", "ORDER_CONSTRUCT": "enum:Buildings", "ORDER_CREATE": "enum:Projects", "ORDER_MAINTAIN": "enum:Processes"}}},
        "bAlt": {"name": "save", "note": "pushOrder bSave"}, "bShift": {"name": "replace", "note": "pushOrder bPop: clear the queue first"},
        "bCtrl": {"name": "append", "note": "pushOrder bAppend"}},
  rules=["applicability/hand: ORDER_TRAIN: CvCity::canTrain(unit); ORDER_CONSTRUCT: canConstruct(building); ORDER_CREATE: canCreate(project); ORDER_MAINTAIN: canMaintain(process). CvCity::pushOrder re-checks them but only after the queue was cleared [gap:AUD-12]",
         "applicability/hand: the host runs the can* check first and rejects an invalid order before it reaches pushOrder, so the replace flag never wipes the queue [gap:AUD-12]"])
C("city.pop_order", "player", send="sendPopOrder", resp="ResponseCityPopOrder", rate="city",
  over={"iNum": {"name": "index"}, "iNum0": {"name": "index"}},
  app="queue index in [0, CvCity::getOrderQueueLength()); handler looks the city up only")
C("city.swap_order", "player", send="sendSwapOrder", resp="ResponseCitySwapOrder", rate="city",
  over={"iNum": {"name": "index"}, "iNum0": {"name": "index"}},
  app="queue index in [0, CvCity::getOrderQueueLength()); handler looks the city up only")
C("city.purchase", "player", send="sendPurchase", resp="ResponseCityPurchase", rate="city",
  over={"ePurchaseYield": {"name": "yield", "type": "enum:Yields", "none": True, "note": "NO_YIELD = gold"},
        "eUnitType": {"none": True}, "eBuildingType": {"none": True}, "eProjectType": {"none": True}},
  app="CvCity::IsCanPurchase / Purchase re-check; handler range-checks the yield in [-1, NUM_YIELD_TYPES). Exactly one of unit/building/project is set")
C("city.do_task", "player", lua="SendDoTask", send="sendDoTask", resp="ResponseCityDoTask", rate="city", gaps=["AUD-19"],
  over={"iData1": {"name": "data1"}, "iData2": {"name": "data2"}, "bOption": {"name": "option"}, "bAlt": {"name": "alt"}, "bShift": {"name": "shift"}, "bCtrl": {"name": "ctrl"}},
  rules=["applicability/hand: per task (CvCity::doTask): RAZE: canRaze; HURRY: canHurry; CONSCRIPT: canConscript; RANGED_ATTACK: canRangeStrikeAt(data1, data2) (each callee re-checks); CHANGE_WORKING_PLOT: data1 in [0, NUM_CITY_PLOTS) (assert only in the callee); SET_EMPHASIZE: emphasize id in range; ADD/REMOVE_SPECIALIST: building id valid and a slot available",
         "applicability/hand: TASK_GIFT and TASK_DISBAND are rejected with not_applicable [gap:AUD-19]: no shipped sender and no check in the DLL (gift: any own city to any player id, unchecked index; disband: deletes any own city)"])
C("city.buy_plot", "player", lua="SendCityBuyPlot", send="sendCityBuyPlot", resp="ResponseCityBuyPlot", rate="city", over={"iX,iY": {"name": "plot", "none": True, "note": "(-1,-1) picks CvCity::GetNextBuyablePlot()"}},
  app="handler checks CvCity::CanBuyPlot(x, y) and a non-NULL plot")
C("city.sell_building", "player", lua="SendSellBuilding", send="SendSellBuilding", resp="ResponseSellBuilding", rate="city",
  app="building in the city (CvCityBuildings::GetNumBuilding > 0) and CvCityBuildings::IsBuildingSellable(building info); handler delegates to DoSellBuilding and then fires the CitySoldBuilding hook")
C("city.set_ai_focus", "player", lua="SendSetCityAIFocus", send="sendSetCityAIFocus", resp="ResponseSetCityAIFocus", rate="city", gaps=["AUD-11"],
  over={"eFocus": {"name": "focus", "none": True}},
  rules=["range/hand: focus in [NO_CITY_AI_FOCUS_TYPE, NUM_CITY_AI_FOCUS_TYPES) [gap:AUD-11]"])
C("city.set_avoid_growth", "player", lua="SendSetCityAvoidGrowth", send="sendSetCityAvoidGrowth", resp="ResponseSetCityAvoidGrowth", rate="city",
  app="handler looks the city up only; forced-avoid-growth is a plain flag")
C("city.update_citizens", "player", lua="SendUpdateCityCitizens", send="SendUpdateCityCitizens", resp="ResponseUpdateCityCitizens", rate="city",
  app="handler looks the city up and re-runs DoVerifyWorkingPlots / DoReallocateCitizens")
C("city.rename", "player", lua="SendRenameCity", send="SendRenameCity", resp="ResponseRenameCity", rate="rename",
  app="handler looks the city up; the name is free text (host limits length and characters)")

# ---------------------------------------------------------------------------------------------------------------
# research, policies, ideology
# ---------------------------------------------------------------------------------------------------------------
C("player.research", "player", send="sendResearch", resp="ResponseResearch", lua="SendResearch", rate="player", tags=[BOT],
  mux={"iDiscover": 0, "ePlayerToStealFrom": "NO_PLAYER"}, drop=["iDiscover", "ePlayerToStealFrom"],
  derived={"iDiscover": 0, "ePlayerToStealFrom": "NO_PLAYER"},
  over={"eTech": {"name": "tech", "none": True, "note": "NO_TECH clears the research queue"}, "bShift": {"name": "append"}},
  rules=["applicability/hand: tech is NO_TECH (clears the queue) or CvPlayerTechs::CanEverResearch(tech); otherwise the handler ignores the command (CanEverResearch runs the Lua hook PlayerCanEverResearch: the host query must be hook-neutral, OQ 4)",
         "phase/hand: allowed whenever the player's turn is active, not only while ENDTURN_BLOCKING_RESEARCH is pending (the queue can be changed any time)"])
C("player.free_tech", "player", send="sendResearch", resp="ResponseResearch", lua="SendResearch", rate="player", tags=[BOT], gaps=["FIX-0101"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_FREE_TECH"],
  mux={"iDiscover": ">0", "ePlayerToStealFrom": "NO_PLAYER"}, drop=["iDiscover", "ePlayerToStealFrom", "bShift"],
  derived={"iDiscover": "CvPlayer::GetNumFreeTechs()", "ePlayerToStealFrom": "NO_PLAYER", "bShift": False},
  over={"eTech": {"name": "tech"}},
  rules=["pending/hand: CvPlayer::GetNumFreeTechs() > 0 [gap:FIX-0101]; the honest sender passes iDiscover == GetNumFreeTechs(), the host fills it, the client never supplies it",
         "applicability/hand: CvPlayerTechs::CanResearchForFree(tech) (the handler already ignores a tech that fails it; it runs the Lua hook PlayerCanResearch, so the host query must be hook-neutral, OQ 4)"])
C("espionage.steal_tech", "player", send="sendResearch", resp="ResponseResearch", lua="SendResearch", rate="player", gaps=["FIX-0101"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_STEAL_TECH"],
  mux={"iDiscover": 0, "ePlayerToStealFrom": "<major>"}, drop=["iDiscover", "bShift"], derived={"iDiscover": 0, "bShift": False},
  over={"eTech": {"name": "tech"}, "ePlayerToStealFrom": {"name": "from_player", **MAJOR}},
  rules=["player/hand: from_player is a major civ in [0, MAX_MAJOR_CIVS) [gap:FIX-0101]",
         "range/hand: tech in [0, NumTechInfos) [gap:FIX-0101]",
         "applicability/hand: CvPlayerEspionage::GetNumTechsToSteal(from_player) > 0 and CvPlayer::canStealTech(from_player, tech) (the handler checks both under BUILD_STEALABLE_TECH_LIST_ONCE_PER_TURN)"])
C("policy.adopt", "player", send="sendUpdatePolicies", resp="ResponseUpdatePolicies", lua="SendUpdatePolicies", rate="player", tags=[BOT],
  mux={"bNOTPolicyBranch": True, "bValue": True}, drop=["bNOTPolicyBranch", "bValue"], derived={"bNOTPolicyBranch": True, "bValue": True},
  over={"iPolicyID": {"name": "policy", "type": "enum:Policies"}, "iID": {"name": "policy", "type": "enum:Policies"}},
  rules=["applicability/hand: CvPlayer::canAdoptPolicy(policy); doAdoptPolicy NULL-checks the policy info and re-checks canAdoptPolicy",
         "phase/hand: culture >= next policy cost or a free policy; ENDTURN_BLOCKING_POLICY / FREE_POLICY are hints, not preconditions"],
  note="Tenets (level > 0 policies) go through the same command; ideology choice is policy.choose_ideology.")
C("policy.unlock_branch", "player", send="sendUpdatePolicies", resp="ResponseUpdatePolicies", lua="SendUpdatePolicies", rate="player",
  mux={"bNOTPolicyBranch": False, "bValue": True}, drop=["bNOTPolicyBranch", "bValue"], derived={"bNOTPolicyBranch": False, "bValue": True},
  over={"iPolicyID": {"name": "branch", "type": "enum:PolicyBranches"}, "iID": {"name": "branch", "type": "enum:PolicyBranches"}},
  rules=["range/hand: branch in [0, NumPolicyBranches): only the unlock path null-checks the branch info, the switch path indexes arrays",
         "applicability/hand: CvPlayerPolicies::CanUnlockPolicyBranch(branch) (DoUnlockPolicyBranch re-checks it); a blocked-but-unlocked branch is switched to"])
C("policy.remove", "unsupported", send="sendUpdatePolicies", resp="ResponseUpdatePolicies", lua="SendUpdatePolicies", gaps=["FIX-0110"],
  mux={"bNOTPolicyBranch": True, "bValue": False}, drop=["bNOTPolicyBranch", "bValue"],
  rules=["class/hand: rejected 'forbidden' [gap:FIX-0110]: no honest sender exists in the shipped Lua (every call passes bValue = true), the engine's own senders cannot be inspected, so the DLL was left as is and the host refuses the command"])
C("policy.choose_ideology", "player", resp="ResponseIdeologyChoice", lua="SendIdeologyChoice", rate="player", tags=[BOT], gaps=["AUD-04"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_CHOOSE_IDEOLOGY"],
  over={"eChoice": {"name": "branch", "type": "enum:PolicyBranches"}},
  note="No send* in ICvEngineUtility: the Lua UI calls Network.SendIdeologyChoice directly (bot table 'sendIdeologyChoice').",
  rules=["pending/hand: ENDTURN_BLOCKING_CHOOSE_IDEOLOGY is pending (NOTIFICATION_CHOOSE_IDEOLOGY) [gap:AUD-04]",
         "applicability/hand: branch is one of the three late-game ideology branches and CanUnlockPolicyBranch(branch) [gap:AUD-04]"])
C("player.change_ideology", "player", send="sendChangeIdeology", resp="ResponseChangeIdeology", lua="SendChangeIdeology", rate="player", gaps=["AUD-10"],
  rules=["applicability/hand: the player has an ideology and CvPlayerCulture::GetPublicOpinionPreferredIdeology() is neither NO_POLICY_BRANCH_TYPE nor the current one (the offer shown by the public-opinion screen) [gap:AUD-10]",
         "applicability/hand: player alive (the handler's only check)"])

# ---------------------------------------------------------------------------------------------------------------
# diplomacy, war
# ---------------------------------------------------------------------------------------------------------------
C("diplo.change_war", "player", send="sendChangeWar", resp="ResponseChangeWar", lua="SendChangeWar", rate="player", gaps=["FIX-0102"],
  over={"eRivalTeam": {"name": "rival_team"}, "iRivalTeam": {"name": "rival_team"}, "bWar": {"name": "war", "note": "true = declare war, false = make peace"}},
  rules=["player/hand: rival_team in [0, MAX_TEAMS) and != the session player's team [gap:FIX-0102]",
         "applicability/hand: war=true: CvTeam::canDeclareWar(rival_team); war=false: CvTeam::canChangeWarPeace(rival_team) [gap:FIX-0102]. NOT in the DLL on purpose: canDeclareWar runs the Lua hook CanDeclareWar (impure) and the city-state popups send ChangeWar without gating, so a DLL rule could reject honest play; the host query must be hook-neutral (OQ 4)",
         "applicability/hand: the same rule applies to diplo.from_ui_event HUMAN_DECLARES_WAR, which declares war through another path (AUD-08)"])
C("diplo.ignore_warning", "player", send="sendIgnoreWarning", resp="ResponseIgnoreWarning", lua="SendIgnoreWarning", rate="player",
  over={"eRivalTeam": {"name": "rival_team"}, "iRivalTeam": {"name": "rival_team"}},
  app="rival_team in [0, MAX_TEAMS) and != own team (the handler only FAsserts it); the handler then calls CvTeam::PushIgnoreWarning")
C("diplo.from_ui_event", "player", send="sendFromUIDiploEvent", resp="ResponseDiplomacyFromUI", rate="player", gaps=["FIX-0106", "AUD-08"],
  over={"eOtherPlayer": {"name": "other_player", **MAJOR}, "eEvent": {"name": "event", "type": "enum:FromUIDiploEvents"}, "iArg1": {"name": "arg1"}, "iArg2": {"name": "arg2"}},
  rules=["player/hand: other_player in [0, MAX_PLAYERS) [gap:FIX-0106]; and alive and a major civ with a CvDiplomacyAI (the DLL rule is the range only, a dead/minor target is not rejected)",
         "range/hand: event in [0, NUM_FROM_UI_DIPLO_EVENTS) [gap:AUD-08]",
         "applicability/hand: event HUMAN_DECLARES_WAR: same rule as diplo.change_war (war=true) [gap:AUD-08]; response events (DEMAND_HUMAN_REFUSAL, *_RESPONSE, ...) require the matching AI request to be pending (OQ 7)"])
C("diplo.vote", "player", send="SendDiploVote", resp="ResponseDiploVote", lua="SendDiploVote", rate="player", gaps=["AUD-05"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_DIPLO_VOTE"],
  over={"eVotePlayer": {"name": "vote_player"}},
  rules=["player/hand: vote_player in [0, MAX_PLAYERS) and alive [gap:AUD-05]",
         "pending/hand: ENDTURN_BLOCKING_DIPLO_VOTE is pending (NOTIFICATION_DIPLO_VOTE)"])
C("diplo.accept_ai_deal", "player", send="sendNetDealAccepted", rate="player",
  exe="net message handled inside the exe (no Response* in ICvNetMessageHandler)",
  derived={"fromPlayer": "AI proposer", "toPlayer": "session.player", "pDeal": "the pending ICvDeal1", "iDealValueToMe": "computed", "iValueImOffering": "computed", "iValueTheyreOffering": "computed"},
  drop=["fromPlayer", "toPlayer", "pDeal", "iDealValueToMe", "iValueImOffering", "iValueTheyreOffering"],
  extra_args=[{"name": "offer", "type": "int", "min": 0, "note": "host-assigned id of the pending offer (OPEN, OQ 8)"}],
  note="OPEN: v2 models it as diplo.respond_ai_offer{offer_id, accept}; the DLL assembles the ICvDeal1 itself.",
  rules=["pending/hand: an AI deal offer to the session player is pending (OPEN, OQ 7/8: popup state, no ENDTURN_BLOCKING type)",
         "applicability/hand: the deal is still valid (CvGameDeals), the bot declines by sending nothing"],
  phase=["turn_active", "pending:ai_deal_offer"])
C("diplo.accept_ai_demand", "player", send="sendNetDemandAccepted", rate="player",
  exe="net message handled inside the exe (no Response* in ICvNetMessageHandler)",
  derived={"fromPlayer": "AI demander", "toPlayer": "session.player", "pDeal": "the pending ICvDeal1"},
  drop=["fromPlayer", "toPlayer", "pDeal"],
  extra_args=[{"name": "offer", "type": "int", "min": 0, "note": "host-assigned id of the pending demand (OPEN, OQ 8)"}],
  note="OPEN: see diplo.accept_ai_deal.",
  rules=["pending/hand: an AI demand to the session player is pending (OPEN, OQ 7/8)", "applicability/hand: the demand is still valid (CvGameDeals)"],
  phase=["turn_active", "pending:ai_demand"])
C("deal.finalize", "player", resp="ResponsePlayerDealFinalized", rate="player", gaps=["AUD-09"],
  over={"eFromPlayer": {"name": "from_player"}, "eToPlayer": {"name": "to_player"}, "eActBy": {"name": "acting"}, "bAccepted": {"name": "accepted"}},
  actor="eActBy",
  note="No send* and no Network.Send* in the shipped Lua: sent by the engine when a deal is finalized (accept/withdraw/reject).",
  rules=["player/hand: from_player and to_player in [0, MAX_PLAYERS), alive, distinct; session.player is one of them and is the actor [gap:AUD-09]",
         "applicability/hand: CvGameDeals::FinalizeDeal(from, to, accepted) refuses an unknown deal itself (the handler then posts 'deal expired' notifications)"])

# ---------------------------------------------------------------------------------------------------------------
# city-states
# ---------------------------------------------------------------------------------------------------------------
_MINOR_RULE = "player/hand: minor is a city-state in [MAX_MAJOR_CIVS, MAX_CIV_PLAYERS) and alive [gap:FIX-0107]"
C("minor.gift_gold", "player", send="sendMinorGiftGold", resp="ResponseMinorCivGiftGold", rate="player", gaps=["FIX-0107", "AUD-01"],
  mux={"iGold": "<=100000"},
  over={"eMinor": {"name": "minor", **MINOR}, "iGold": {"name": "gold", "min": 1, "max": 100000, "reserved_above": 100000, "note": "wire values above 100000 are the faith multiplexer (minor.gift_faith)"}},
  rules=[_MINOR_RULE,
         "range/hand: 1 <= gold <= 100000; gold < 0 would credit the sender, gold > 100000 is reserved_value (use minor.gift_faith) [gap:AUD-01]",
         "applicability/hand: GetTreasury()->GetGold() >= gold (the handler checks it); CvMinorCivAI::DoGoldGiftFromMajor refuses bribes of an ally past NQ_NUM_TURNS_BEFORE_MINOR_ALLIES_REFUSE_BRIBES"])
C("minor.gift_faith", "lekmod_mp", send="sendMinorGiftGold", resp="ResponseMinorCivGiftGold", rate="player", gaps=["FIX-0107"],
  mux={"iGold": ">100000"}, drop=["iGold"],
  derived={"iGold": "faith + 100000"},
  over={"eMinor": {"name": "minor", **MINOR}},
  extra_args=[{"name": "faith", "type": "int", "min": 1, "note": "wire iGold = faith + 100000 (NQ_BELIEF_TOGGLE_ALLOW_FAITH_GIFTS_TO_MINORS)"}],
  rules=[_MINOR_RULE, "applicability/hand: GetFaith() >= faith (the handler checks it); only when the faith-gift belief toggle is on"])
C("minor.bully_gold", "player", send="sendMinorBullyGold", resp="ResponseMinorCivBullyGold", rate="player", gaps=["FIX-0107", "AUD-13"],
  drop=["eBully", "iGold"], derived={"iGold": "CvMinorCivAI::GetBullyGoldAmount(session.player)"},
  over={"eMinor": {"name": "minor", **MINOR}},
  rules=[_MINOR_RULE,
         "applicability/hand: CvMinorCivAI::CanMajorBullyGold(session.player, CalculateBullyMetric(player, false)); DoMajorBullyGold evaluates it itself",
         "range/hand: the client sends no amount: the host fills iGold with CvMinorCivAI::GetBullyGoldAmount(session.player), exactly what CvGame::DoMinorBullyGold sends; the handler credits whatever the message carries [gap:AUD-13]"])
C("minor.bully_unit", "player", send="sendMinorBullyUnit", resp="ResponseMinorCivBullyUnit", rate="player", gaps=["FIX-0107", "AUD-14"],
  drop=["eBully", "eUnitType"], derived={"eUnitType": "UNIT_WORKER"},
  over={"eMinor": {"name": "minor", **MINOR}},
  rules=[_MINOR_RULE,
         "applicability/hand: CvMinorCivAI::CanMajorBullyUnit(session.player, CalculateBullyMetric(player, true)); DoMajorBullyUnit evaluates it itself and needs the city-state's capital",
         "range/hand: the client sends no unit type: the host fills UNIT_WORKER, the only value CvGame::DoMinorBullyUnit ever sends; the handler spawns whatever type the message carries [gap:AUD-14]"])
C("minor.gift_tile_improvement", "player", send="sendMinorGiftTileImprovement", resp="ResponseMinorCivGiftTileImprovement", rate="player", gaps=["FIX-0107"],
  over={"eMinor": {"name": "minor", **MINOR}, "iPlotX,iPlotY": {"name": "plot"}}, drop=["eMajor"],
  rules=[_MINOR_RULE, "applicability/hand: plot on the map and CvMinorCivAI::CanMajorGiftTileImprovementAtPlot(session.player, x, y); DoTileImprovementGiftFromMajor re-checks it and NULL-checks the plot"])
C("minor.buyout", "player", send="sendMinorBuyout", resp="ResponseMinorCivBuyout", rate="player", gaps=["FIX-0107"],
  over={"eMinor": {"name": "minor", **MINOR}}, drop=["eMajor"],
  rules=[_MINOR_RULE, "applicability/hand: CvMinorCivAI::CanMajorBuyout(session.player); DoBuyout re-checks it and the major range"])
C("minor.pledge_protection", "player", send=["sendMinorPledgeProtection", "SendPledgeMinorProtection"], resp="ResponsePledgeMinorProtection", lua="SendPledgeMinorProtection", rate="player", gaps=["FIX-0107"],
  over={"eMinor": {"name": "minor", **MINOR}, "bValue": {"name": "protect"}, "bProtect": {"name": "protect"}, "bPledgeNowBroken": {"name": "pledge_now_broken"}}, drop=["eMajor"],
  rules=[_MINOR_RULE, "applicability/hand: protect=true: CvMinorCivAI::CanMajorStartProtection(session.player) (CanMajorProtect and not already protecting; DoChangeProtectionFromMajor re-checks CanMajorProtect and the major range). SendPledgeMinorProtection is deprecated (antonjs), kept as an alias of the same wire message"])
C("minor.no_unit_spawning", "player", send="SendMinorNoUnitSpawning", resp="ResponseMinorNoUnitSpawning", lua="SendMinorNoUnitSpawning", rate="player", gaps=["FIX-0107"],
  over={"eMinor": {"name": "minor", **MINOR}, "bValue": {"name": "disabled"}},
  rules=[_MINOR_RULE])
C("minor.liberate", "player", send="SendLiberateMinor", resp="ResponseLiberatePlayer", lua="SendLiberateMinor", rate="player", gaps=["AUD-16", "AUD-18"],
  over={"eMinor": {"name": "liberated", "note": "player whose captured city is returned"}, "eLiberatedPlayer": {"name": "liberated"}, "iCityID": {"name": "city"}},
  rules=["applicability/hand: CvPlayer::CanLiberatePlayerCity(liberated) holds for the city [gap:AUD-16] (DoLiberatePlayer only NULL-checks the city)",
         "player/hand: liberated in [0, MAX_PLAYERS) [gap:AUD-18]"])
C("minor.gift_unit", "player", send="sendGiftUnit", resp="ResponseGiftUnit", lua="SendGiftUnit", rate="player", gaps=["AUD-06"],
  mux={"iUnitID": ">=0"},
  over={"eMinor": {"name": "minor", **MINOR}, "iUnitID": {"name": "unit", "reserved_below": 0, "note": "negative ids are the Lekmod multiplexer (mp.*, stats.*, server.autopause_*)"}},
  rules=["range/hand: unit id < 0 is reserved_value (mp.turn_timer_reset -1, mp.propose_irr -2, mp.propose_cc -3, mp.propose_scrap -4, mp.vote -5/-6, mp.turn_timer_pause -7, stats.demographics_opened -8, server -10/-11)",
         "player/hand: minor is a city-state and alive [gap:AUD-06]",
         "applicability/hand: CvPlayer::DoDistanceGift ignores a missing unit or plot; the host additionally requires the unit to be a gift-able unit of the session player"])
C("civilian.return", "player", lua="SendReturnCivilian", send="sendReturnCivilian", resp="ResponseReturnCivilian", rate="player", gaps=["AUD-15"],
  over={"eToPlayer": {"name": "to_player"}, "ePlayer": {"name": "to_player"}, "bReturn": {"name": "return_to_owner"}},
  rules=["ownership/hand: unit exists and owner == session.player (CvPlayer::DoCivilianReturnLogic only looks the unit up)",
         "pending/hand: the unit is a captured civilian whose return/keep decision is open and to_player is its previous owner [gap:AUD-15]; the DLL checks neither, the host must record the capture event (popup state, OPEN, OQ 7)",
         "player/hand: to_player in [0, MAX_PLAYERS) and alive [gap:AUD-15]"],
  phase=["turn_active", "pending:return_civilian_popup"])
C("barbarian.ransom", "player", send="sendBarbarianRansom", resp="ResponseBarbarianRansom", lua="SendBarbarianRansom", rate="player", gaps=["FIX-0103"],
  phase=["turn_active", "pending:barbarian_ransom_popup"],
  over={"iOptionChosen": {"name": "option", "min": 0, "max": 1, "note": "0 = pay the ransom, 1 = abandon the unit"}},
  rules=["pending/hand: a ransom for this unit was offered this turn [gap:FIX-0103]. The DLL keeps no pending-ransom state (the engine opens BUTTONPOPUP_BARBARIAN_RANSOM) so a DLL rule is impossible without a save-format change; the v2 host must record the offer when the capture happens (source OPEN, OQ 7)",
         "ownership/hand: unit exists and owner == session.player (option 1 kills it)"])

# ---------------------------------------------------------------------------------------------------------------
# religion, great people, goody huts, culture
# ---------------------------------------------------------------------------------------------------------------
C("religion.found_pantheon", "player", send="SendFoundPantheon", resp="ResponseFoundPantheon", lua="SendFoundPantheon", rate="player", tags=[BOT],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_FOUND_PANTHEON"], over={"eBelief": {"name": "belief"}},
  rules=["applicability/hand: belief is a pantheon belief and CvGameReligions::CanCreatePantheon(player, true) == FOUNDING_OK and IsPantheonBeliefAvailable(belief) (the handler checks all three; a reformation belief goes through CanAddReformationBelief)"])
C("religion.found", "player", send="SendFoundReligion", resp="ResponseFoundReligion", lua="SendFoundReligion", rate="player", tags=[BOT], gaps=["FIX-0104"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_FOUND_RELIGION"],
  over={"eReligion": {"name": "religion"}, "szCustomName": {"name": "custom_name", "optional": True}, "eBelief1": {"name": "belief1"}, "eBelief2": {"name": "belief2"}, "eBelief3": {"name": "belief3", "none": True}, "eBelief4": {"name": "belief4", "none": True}, "iCityX,iCityY": {"name": "city_plot"}},
  rules=["range/hand: city_plot inside map bounds [gap:FIX-0104]",
         "ownership/hand: a city on city_plot exists and owner == session.player [gap:FIX-0104] (the holy city)",
         "applicability/hand: CvGameReligions::CanFoundReligion(player, religion, name, b1, b2, b3, b4, city) == FOUNDING_OK (handler); on failure the handler re-posts the found-religion notification"])
C("religion.enhance", "player", send="SendEnhanceReligion", resp="ResponseEnhanceReligion", lua="SendEnhanceReligion", rate="player", tags=[BOT], gaps=["FIX-0105"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_ENHANCE_RELIGION"],
  mux={"eReligion": ">=0"},
  over={"eReligion": {"name": "religion", "reserved_below": 0, "note": "wire -1 is the replay-chat multiplexer (replay.chat)"}, "szCustomName": {"name": "custom_name", "optional": True},
        "eBelief1": {"name": "belief1"}, "eBelief2": {"name": "belief2"}, "iCityX,iCityY": {"name": "city_plot", "none": True, "note": "(-1,-1) allowed: only the failure branch reads it"}},
  rules=["range/hand: religion id >= 0: -1 is reserved_value (replay.chat) [gap:FIX-0105]",
         "range/hand: city_plot is on the map or (-1,-1); the DLL deliberately does not validate it up front (honest UI may send (-1,-1)) and only null-checks it in the failure branch [gap:FIX-0105]",
         "applicability/hand: CvGameReligions::CanEnhanceReligion(player, religion, b1, b2) == FOUNDING_OK (handler); the founder check lives inside it"])
C("replay.chat", "meta", send="SendEnhanceReligion", resp="ResponseEnhanceReligion", lua="SendEnhanceReligion", rate="chat", phase=["any"],
  mux={"eReligion": -1}, drop=["eReligion", "eBelief3", "eBelief4", "iCityX", "iCityY", "iCityX,iCityY"],
  derived={"eReligion": -1, "iCityX": -1, "iCityY": -1},
  over={"szCustomName": {"name": "text", "max_len": 127, "note": "Lekmod UI cuts text:sub(1,127)"}, "eBelief1": {"name": "target_type", "type": "int", "note": "iTargetType (chat target kind)"}, "eBelief2": {"name": "to", "type": "int", "note": "iToPlayerOrTeam"}},
  rules=["range/hand: text is non-empty UTF-8 without control characters, <= 127 bytes",
         "player/hand: when target_type addresses a player or team, 'to' is a valid id; chat goes to the replay only (REPLAY_MESSAGE_EXTENDED), no game state"])
C("religion.faith_purchase", "player", send="SendFaithPurchase", resp="ResponseFaithPurchase", lua="SendFaithPurchase", rate="player",
  over={"eFaithPurchaseType": {"name": "purchase_type"}, "iFaithPurchaseIndex": {"name": "index"}},
  app="handler only stores the automatic-faith-purchase preference (CvPlayer::SetFaithPurchaseType/Index); CvGameReligions consumes it when the player can afford it, so nothing is bought by this command alone")
C("gp.choose", "player", lua="SendGreatPersonChoice", send="sendGreatPersonChoice", resp="ResponseGreatPersonChoice", rate="player", tags=[BOT], gaps=["AUD-03"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_FREE_ITEMS"], over={"eGreatPersonUnit": {"name": "unit_type"}, "eUnit": {"name": "unit_type"}},
  rules=["pending/hand: CvPlayer::GetNumFreeGreatPeople() > 0 [gap:AUD-03] (the handler spawns only then, but decrements the counter regardless)",
         "applicability/hand: CvPlayer::GetGreatPersonSpawnCity(unit_type) != NULL and unit_type is a great person"])
C("gp.maya_bonus", "player", lua="SendMayaBonusChoice", send="sendMayaBonusChoice", resp="ResponseMayaBonusChoice", rate="player", tags=[BOT], gaps=["AUD-03"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_MAYA_LONG_COUNT"], over={"eGreatPersonUnit": {"name": "unit_type"}, "eUnit": {"name": "unit_type"}},
  rules=["pending/hand: CvPlayer::GetNumMayaBoosts() > 0 [gap:AUD-03]", "applicability/hand: CvPlayer::GetGreatPersonSpawnCity(unit_type) != NULL"])
C("gp.faith_choose", "player", lua="SendFaithGreatPersonChoice", send="sendFaithGreatPersonChoice", resp="ResponseFaithGreatPersonChoice", rate="player", tags=[BOT], gaps=["AUD-02"],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_FAITH_GREAT_PERSON"], over={"eGreatPersonUnit": {"name": "unit_type"}, "eUnit": {"name": "unit_type"}},
  rules=["pending/hand: CvPlayer::GetNumFaithGreatPeople() > 0 [gap:AUD-02]", "applicability/hand: CvPlayer::GetGreatPersonSpawnCity(unit_type) != NULL"])
C("goody.choose", "player", send="sendGoodyChoice", resp="ResponseGoodyChoice", lua="SendGoodyChoice", rate="player", tags=[BOT], gaps=["FIX-0108"],
  phase=["turn_active", "pending:goody_hut_picker"],
  over={"iPlotX,iPlotY": {"name": "plot"}, "eGoody": {"name": "goody"}, "iUnitID": {"name": "unit"}},
  rules=["ownership/hand: unit exists, owner == session.player and carries PROMOTION_GOODY_HUT_PICKER (the only unit for which CvPlayer::doGoody opens the choice) [gap:FIX-0108]",
         "range/hand: plot exists and goody is a GoodyHuts Type (id >= 0) [gap:FIX-0108]",
         "pending/hand: the DLL keeps no pending-choice state (the hut is removed before the popup opens) and canReceiveGoody() runs the Lua hook GoodyHutCanResearch, so it is not used; residual hole: a picker unit can pick when no hut is pending. The v2 host closes it by recording the picker event"])
C("culture.archaeology_choice", "player", resp="ResponseArchaeologyChoice", lua="SendArchaeologyChoice", rate="player", tags=[BOT],
  phase=["turn_active", "pending:ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY"], over={"eChoice": {"name": "choice", "type": "enum:ArchaeologyChoices"}},
  note="No send* in ICvEngineUtility: the Lua popup calls Network.SendArchaeologyChoice.",
  app="handler forwards to CvPlayerCulture::DoArchaeologyChoice, which uses the pending dig stored in the player; host requires ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY")
C("culture.set_swappable_great_work", "player", send="sendSetSwappableGreatWork", resp="ResponseSetSwappableGreatWork", lua="SendSetSwappableGreatWork", rate="player",
  over={"iWorkClass": {"name": "work_class", "type": "enum:GreatWorkClasses"}, "iWorkIndex": {"name": "work_index", "none": True}},
  app="handler requires an alive player and CvPlayerCulture::ControlsGreatWork(work_index) (index -1 clears the slot)")
C("culture.swap_great_works", "player", send="sendSwapGreatWorks", resp="ResponseSwapGreatWorks", lua="SendSwapGreatWorks", rate="player", actor="ePlayer1", gaps=["AUD-18"],
  over={"ePlayer1": {"name": "player"}, "iWork1": {"name": "work_index1"}, "iWorkIndex1": {"name": "work_index1"}, "ePlayer2": {"name": "other_player"}, "PlayerTypes2": {"name": "other_player"}, "iWork2": {"name": "work_index2"}, "iWorkIndex2": {"name": "work_index2"}},
  rules=["player/hand: other_player in [0, MAX_PLAYERS) and alive [gap:AUD-18] (SwapGreatWorks checks only NO_PLAYER before GET_PLAYER)",
         "applicability/hand: both works exist, are of the same class and controlled/marked swappable (CvGameCulture::SwapGreatWorks checks alive and class itself)"])
C("culture.move_great_works", "player", send="sendMoveGreatWorks", resp="ResponseMoveGreatWorks", lua="SendMoveGreatWorks", rate="player", gaps=["AUD-17"],
  over={"iCity1": {"name": "city1"}, "iCity2": {"name": "city2"}, "iBuildingClass1": {"name": "building_class1", "type": "enum:BuildingClasses"}, "iBuildingClass2": {"name": "building_class2", "type": "enum:BuildingClasses"}, "iWorkIndex1": {"name": "work_index1"}, "iWorkIndex2": {"name": "work_index2"}},
  rules=["ownership/hand: both cities exist and are owned by the session player (MoveGreatWorks looks them up in the player's own list)",
         "applicability/hand: building class valid, slot index inside the building's slots, both slots take the work's slot type [gap:AUD-17]"])

# ---------------------------------------------------------------------------------------------------------------
# espionage, spaceship, misc player
# ---------------------------------------------------------------------------------------------------------------
C("espionage.move_spy", "player", send="SendMoveSpy", resp="ResponseMoveSpy", lua="SendMoveSpy", rate="player", gaps=["AUD-18"],
  over={"iSpyIndex": {"name": "spy"}, "iTargetPlayer": {"name": "target_player", "type": "player", "none": True}, "iTargetCity": {"name": "target_city", "none": True}, "bAsDiplomat": {"name": "as_diplomat"}},
  rules=["player/hand: target_player in [0, MAX_PLAYERS) and alive unless target_city == -1 (extract the spy) [gap:AUD-18]: the handler only checks != -1 and then indexes GET_PLAYER",
         "applicability/hand: spy index < spy count (ExtractSpyFromCity/MoveSpyTo check it) and CvPlayerEspionage::CanMoveSpyTo(city, spy, as_diplomat) (MoveSpyTo re-checks)"])
C("espionage.stage_coup", "player", send="SendStageCoup", resp="ResponseStageCoup", lua="SendStageCoup", rate="player", actor="eSpyPlayer",
  over={"iSpyIndex": {"name": "spy"}},
  app="spy index < spy count and CvPlayerEspionage::CanStageCoup(spy) (AttemptCoup re-checks both)")
C("player.launch_spaceship", "player", send="sendLaunch", resp="ResponseLaunchSpaceship", rate="player", drop=["ePlayer"],
  over={"eVictory": {"name": "victory"}},
  app="handler checks CvTeam::canLaunch(victory) before CvPlayer::launch")
C("player.set_option", "meta", send="sendPlayerOption", resp="ResponsePlayerOption", rate="player", gaps=["AUD-07"],
  over={"eOption": {"name": "option"}, "bValue": {"name": "value"}},
  rules=["range/hand: option is a PlayerOptions Type (id in [0, NUM_PLAYEROPTION_TYPES)) [gap:AUD-07]"],
  phase=["any"])
C("player.extended_game", "player", send="sendExtendedGame", resp="ResponseExtendedGame", lua="SendExtendedGame", rate="player", phase=["any"],
  app="handler only sets m_bExtendedGame; host requires the game state to be GAMESTATE_OVER for the session player (CvGame::testExtendedGame)")

# ---------------------------------------------------------------------------------------------------------------
# world congress
# ---------------------------------------------------------------------------------------------------------------
_LG = ["FIX-0109"]
C("league.vote_enact", "player", send="SendLeagueVoteEnact", resp="ResponseLeagueVoteEnact", lua="SendLeagueVoteEnact", rate="player", gaps=_LG,
  actor="eVoter", phase=["turn_active", "pending:ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES"],
  over={"eLeague": {"name": "league"}, "iResolutionID": {"name": "resolution_id"}, "iNumVotes": {"name": "num_votes", "min": 0}, "iChoice": {"name": "choice"}},
  rules=["range/hand: league exists, num_votes >= 0 (negative adds votes), resolution id valid [gap:FIX-0109]",
         "applicability/hand: CvLeague::CanVote(session.player) (CvAssertMsg only in golden) and num_votes <= the player's votes [gap:FIX-0109]"])
C("league.vote_repeal", "player", send="SendLeagueVoteRepeal", resp="ResponseLeagueVoteRepeal", lua="SendLeagueVoteRepeal", rate="player", gaps=_LG,
  actor="eVoter", phase=["turn_active", "pending:ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES"],
  over={"eLeague": {"name": "league"}, "iResolutionID": {"name": "resolution_id"}, "iNumVotes": {"name": "num_votes", "min": 0}, "iChoice": {"name": "choice"}},
  rules=["range/hand: league exists, num_votes >= 0, resolution id valid [gap:FIX-0109]", "applicability/hand: CvLeague::CanVote(session.player) [gap:FIX-0109]"])
C("league.vote_abstain", "player", send="SendLeagueVoteAbstain", resp="ResponseLeagueVoteAbstain", lua="SendLeagueVoteAbstain", rate="player", gaps=_LG,
  actor="eVoter", phase=["turn_active", "pending:ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES"],
  over={"eLeague": {"name": "league"}, "iNumVotes": {"name": "num_votes", "min": 0}},
  rules=["range/hand: league exists, num_votes >= 0 [gap:FIX-0109]", "applicability/hand: CvLeague::CanVote(session.player) [gap:FIX-0109]"])
C("league.propose_enact", "player", send="SendLeagueProposeEnact", resp="ResponseLeagueProposeEnact", lua="SendLeagueProposeEnact", rate="player", gaps=_LG,
  actor="eProposer", phase=["turn_active", "pending:ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS"],
  over={"eLeague": {"name": "league"}, "eResolution": {"name": "resolution"}, "iChoice": {"name": "choice"}},
  rules=["range/hand: league exists, resolution in [0, NumResolutionInfos) [gap:FIX-0109]", "applicability/hand: CvLeague::CanProposeEnact(resolution, session.player, choice) [gap:FIX-0109]"])
C("league.propose_repeal", "player", send="SendLeagueProposeRepeal", resp="ResponseLeagueProposeRepeal", lua="SendLeagueProposeRepeal", rate="player", gaps=_LG,
  actor="eProposer", phase=["turn_active", "pending:ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS"],
  over={"eLeague": {"name": "league"}, "iResolutionID": {"name": "resolution_id"}},
  rules=["range/hand: league exists [gap:FIX-0109]", "applicability/hand: CvLeague::CanProposeRepeal(resolution_id, session.player) [gap:FIX-0109]"])
C("league.edit_name", "player", send="SendLeagueEditName", resp="ResponseLeagueEditName", lua="SendLeagueEditName", rate="rename", gaps=_LG,
  actor="ePlayer", over={"eLeague": {"name": "league"}, "szCustomName": {"name": "name"}},
  rules=["range/hand: league exists [gap:FIX-0109]", "applicability/hand: CvLeague::CanChangeCustomName(session.player) (DoChangeCustomName checks it itself)"])

# ---------------------------------------------------------------------------------------------------------------
# turn control and chat
# ---------------------------------------------------------------------------------------------------------------
C("turn.complete", "turn", send="sendTurnComplete", rate="turn", tags=[BOT],
  exe="executed by the exe (NetTurnComplete); no Response*",
  rules=["pending/hand: no end-turn blocker is pending (CvNotifications::GetEndTurnBlockedType == NO_ENDTURN_BLOCKING_TYPE, no promotable/ready unit); the exe-side acceptance rule of NetTurnComplete is not reverse-engineered in V1, the host refuses it as 'pending_decision' (bot rule: 'no pending decisions -> sendTurnComplete')",
         "phase/hand: not already sent this turn (gDLL->HasSentTurnComplete())"])
C("turn.unready", "turn", send="sendTurnUnready", lua="SendTurnUnready", rate="turn",
  exe="executed by the exe (NetTurnUnready); no Response*",
  app="TurnComplete was sent this turn and the turn has not advanced yet (the Lua UI uses the boolean result)")
C("turn.auto_moves", "turn", send="sendAutoMoves", rate="turn",
  exe="executed by the exe (NetAutoMoves); no Response*",
  app="player has automated units (CvPlayer::hasAutoUnit) and none were sent yet this turn")
C("chat.send", "meta", send="sendChat", lua="SendChat", rate="chat",
  exe="executed by the exe (NetChat); no Response*", phase=["any"],
  over={"szChatString": {"name": "text", "max_len": 255}, "eTargetType": {"name": "target_type", "type": "enum:ChatTargets"}, "toWho": {"name": "to", "none": True}},
  rules=["range/hand: text non-empty UTF-8 without control characters, <= 255 bytes (proposed)", "player/hand: to is a valid human player when target_type == CHATTARGET_PLAYER (-4); team chat only to the sender's team"])
C("chat.ping", "meta", send="sendPing", rate="chat", exe="executed by the exe (NetPing); no Response*", phase=["any"],
  over={"iX,iY": {"name": "plot"}}, app="plot (x, y) inside map bounds")
C("game.pause", "meta", send="sendPause", rate="chat", exe="executed by the exe (NetPause); no Response*", phase=["any"],
  over={"iPauseID": {"name": "pause_id", "none": True}},
  app="host policy: pause requests are session-rate-limited; -1 is the default 'toggle'; the Lekmod turn-timer pause is mp.turn_timer_pause")

# ---------------------------------------------------------------------------------------------------------------
# Lekmod multiplexers: dedicated commands
# ---------------------------------------------------------------------------------------------------------------
_MUX = dict(send="sendGiftUnit", resp="ResponseGiftUnit", lua="SendGiftUnit", rate="player", drop=["eMinor", "iUnitID"])
C("mp.turn_timer_reset", "lekmod_mp", mux={"iUnitID": -1}, derived={"eMinor": "NO_PLAYER", "iUnitID": -1}, tags=[BOT], **_MUX,
  app="only with GAMEOPTION_END_TURN_TIMER_ENABLED (handler checks); recalculates city-state priorities and resets the turn timer for everybody")
C("mp.propose_irr", "lekmod_mp", mux={"iUnitID": -2}, derived={"eMinor": "NO_PLAYER", "iUnitID": -2}, **_MUX,
  app="MP_PLAYERS_VOTING_SYSTEM: adds a PROPOSAL_IRR by the session player; host: multiplayer game only, one open proposal per kind (CvMPVotingSystem::AddProposal decides)")
C("mp.propose_cc", "lekmod_mp", mux={"iUnitID": -3}, gaps=["AUD-06"], **{**_MUX, "drop": ["iUnitID"]}, derived={"iUnitID": -3},
  over={"eMinor": {"name": "target", "note": "the player the proposal is about (wire eMinor)"}},
  rules=["player/hand: target in [0, MAX_PLAYERS) and alive [gap:AUD-06] (the handler prints GET_PLAYER(target).getName())",
         "applicability/hand: multiplayer game with MP_PLAYERS_VOTING_SYSTEM; CvMPVotingSystem::AddProposal(PROPOSAL_CC, session.player, target)"])
C("mp.propose_scrap", "lekmod_mp", mux={"iUnitID": -4}, derived={"eMinor": "NO_PLAYER", "iUnitID": -4}, **_MUX,
  app="MP_PLAYERS_VOTING_SYSTEM: adds a PROPOSAL_SCRAP by the session player (CvMPVotingSystem::AddProposal decides)")
C("mp.vote", "lekmod_mp", mux={"iUnitID": [-5, -6]}, **{**_MUX, "drop": ["iUnitID"]}, derived={"iUnitID": "-5 if yes else -6"},
  over={"eMinor": {"name": "proposal", "type": "int", "min": 0, "note": "proposal id (wire eMinor, cast to int)"}},
  extra_args=[{"name": "yes", "type": "bool"}],
  rules=["range/hand: proposal is an open proposal id (CvMPVotingSystem::DoVote(id, player, yes) does not validate it in the handler)",
         "applicability/hand: the session player has not voted on it yet and is allowed to (CvMPVotingSystem::DoVote decides)"])
C("mp.turn_timer_pause", "lekmod_mp", mux={"iUnitID": -7}, derived={"eMinor": "NO_PLAYER", "iUnitID": -7}, **_MUX,
  app="only with GAMEOPTION_END_TURN_TIMER_ENABLED; toggles pause/unpause of the turn timer (handler flips m_bIsPaused); host: session-rate-limited, pausing is a server policy")
C("stats.demographics_opened", "lekmod_mp", mux={"iUnitID": -8}, derived={"eMinor": "NO_PLAYER", "iUnitID": -8}, **_MUX,
  app="ENHANCED_GRAPHS counter (CvPlayer::ChangeNumTimesOpenedDemographics(1)); no game effect, rate-limit only")
C("server.autopause_on", "server", mux={"iUnitID": -10}, derived={"eMinor": "NO_PLAYER", "iUnitID": -10}, **_MUX,
  note="Sent by the DLL itself (CvPlayer.cpp) when a player disconnects (GAME_AUTOPAUSE_ON_ACTIVE_DISCONNECT_IF_NOT_SEQUENTIAL).")
C("server.autopause_off", "server", mux={"iUnitID": -11}, derived={"eMinor": "NO_PLAYER", "iUnitID": -11}, **_MUX,
  note="Sent by the DLL itself (CvPlayer.cpp) when the disconnected player is back.")

# ---------------------------------------------------------------------------------------------------------------
# server-only, pregame, unsupported
# ---------------------------------------------------------------------------------------------------------------
C("server.sync_unit", "server", send="sendUnitSyncCheck", note="Sync-check traffic generated by the DLL/exe; never from a client.")
C("server.sync_plot", "server", send="sendPlotSyncCheck", note="Sync-check traffic generated by the DLL/exe; never from a client.")
C("server.sync_city", "server", send="sendCitySyncCheck", note="Sync-check traffic generated by the DLL/exe; never from a client.")
C("server.sync_player", "server", send="sendPlayerSyncCheck", note="Sync-check traffic generated by the DLL/exe; never from a client.")
C("server.sync_rng", "server", send="sendRandomNumberGeneratorSyncCheck", note="Sync-check traffic generated by the DLL/exe; never from a client.")
C("server.ai_civs_processed", "server", send="SendAICivsProcessed", note="Barrier message sent by the DLL after the AI turns (NetAICivsProcessed).")
C("server.player_initial_ai_processed", "server", send="sendPlayerInitialAIProcessed", note="Barrier message sent by the DLL (initial AI processing).")
C("server.game_do_turn_processed", "server", send="SendGameDoTurnProcessed", note="Barrier message sent by the DLL after CvGame::doTurn.")
C("server.turn_reminder", "server", send="sendTurnReminder", note="Reminder to a slow player, generated by the server.")
C("pregame.player_info", "pregame", send="sendPlayerInfo", note="Lobby/staging message; outside the game protocol.")
C("pregame.game_info", "pregame", send="sendGameInfo", note="Lobby/staging message; outside the game protocol.")
C("pregame.game_options", "pregame", lua="SendGameOptions", note="Lua Network.SendGameOptions from the staging room (Lekmod UI); lobby only.")
C("advanced_start.action", "unsupported", send="sendAdvancedStartAction", resp="ResponseAdvancedStartAction", note="Advanced start is not used by Lekmod games.")
C("pbem.turn", "unsupported", send="sendPbemTurn", note="Play-by-email is not supported.")
C("election.choose", "unsupported", resp="ResponseChooseElection", note="Empty handler ('Unused') in the DLL.")
C("player.hurry", "unsupported", send="sendPlayerHurry", note="No Response* and no Lua sender: handled inside the exe, semantics not reverse-engineered in V1.")
C("minor.quest_no_interest", "unsupported", send="sendMinorCivQuestNoInterest", note="Exe-handled message (no Response*), semantics not reverse-engineered in V1.")
C("minor.quest_completed", "unsupported", send="sendMinorCivQuestCompleted", note="Exe-handled message (no Response*), semantics not reverse-engineered in V1.")
C("minor.intrusion", "unsupported", send="sendMinorCivIntrusion", note="Exe-handled message (no Response*), semantics not reverse-engineered in V1.")
C("minor.enter_territory", "unsupported", send="sendMinorCivEnterTerritory", lua="SendMinorCivEnterTerritory", note="Exe-handled message (no Response*), semantics not reverse-engineered in V1.")

# ---------------------------------------------------------------------------------------------------------------
# helpers (client/SDK side, expand to wire commands)
# ---------------------------------------------------------------------------------------------------------------
C("unit.move_to", "helper", expands_to=["unit.push_mission"], tags=[BOT],
  note="push_mission(unit, MISSION_MOVE_TO, x, y, flags, append)",
  args=[{"name": "unit", "type": "unit_ref", "own": True}, {"name": "plot", "type": "plot"}, {"name": "append", "type": "bool"}])
C("unit.found_city", "helper", expands_to=["unit.push_mission"], tags=[BOT],
  note="push_mission(unit, MISSION_FOUND, 0, 0, 0, false); settlers: unit.move_to then unit.found_city",
  args=[{"name": "unit", "type": "unit_ref", "own": True}])
C("unit.move_group", "helper", expands_to=["unit.push_mission"],
  note="N x push_mission(MISSION_MOVE_TO) with the same target",
  args=[{"name": "units", "type": "list:unit_ref", "own": True}, {"name": "plot", "type": "plot"}])
C("player.research_queue", "helper", expands_to=["player.research"],
  note="player.research(first, append=false) then player.research(t, append=true) for the rest",
  args=[{"name": "techs", "type": "list:enum:Technologies"}])
