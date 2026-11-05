-- Lockstep harness bot: plays the local human slot of ONE civhost peer through the paths the real UI uses
-- (Network.Send*, Game.SelectionListGameNetMessage / CityPushOrder / DoControl, UI.SelectUnit / SelectCity,
-- Game.DoMinor*). Loaded by `civhost {"cmd":"bot"}` into its own Lua thread; the host calls
-- HarnessBotTick(frame, mode) between game-core updates with the deterministic heap paused.
--
-- Randomness comes ONLY from the Park-Miller generator below (seeded per peer by the coordinator); the game RNG and
-- math.random are never touched. The policy is "plausible play": found cities, defend, expand, and otherwise pick random
-- but legal options among what the UI would list, using the same Lua queries the UI uses. A configurable share of
-- commands is deliberately illegal or stale (dead unit ids, unavailable techs, far-away targets) to exercise the
-- validation in the net message handler.
--
-- Config (HarnessBotConfig, set by the init chunk): seed, density (actions per turn = density * (units + cities + 3)),
-- illegal (share of bad commands), ui_queries (read-only UI getters on this peer, see UiQueries), stuck_frames.

local cfg = HarnessBotConfig or {}
local seed = math.floor(tonumber(cfg.seed) or 1) % 2147483646 + 1
local density = tonumber(cfg.density) or 1.0
local illegalShare = tonumber(cfg.illegal) or 0.03
local uiQueries = cfg.ui_queries and true or false
local stuckFrames = tonumber(cfg.stuck_frames) or 400

-- ---- host-side PRNG (Park-Miller minimal standard; exact in doubles) ----------------------------------------------------
local function rnd()
	seed = (seed * 16807) % 2147483647
	return seed
end
local function rint(n)	-- 0..n-1
	if n <= 0 then return 0 end
	return rnd() % n
end
local function chance(p) return (rnd() % 1000000) < p * 1000000 end
local function pick(t)
	if #t == 0 then return nil end
	return t[rint(#t) + 1]
end

-- ---- enums ---------------------------------------------------------------------------------------------------------------
local function M(name) return MissionTypes and MissionTypes[name] or (GameInfo.Missions[name] and GameInfo.Missions[name].ID) end
local GM = GameMessageTypes or {}
local CT = CommandTypes or {}
local OT = OrderTypes or {}
local ETB = EndTurnBlockingTypes or {}
local BPT = ButtonPopupTypes or {}
local YT = YieldTypes or {}
local CONTROL = ControlTypes or {}
local AUTOMATE_BUILD, AUTOMATE_EXPLORE = 0, 1

local log = {}
local function note(s) log[#log + 1] = s end

-- ---- state ---------------------------------------------------------------------------------------------------------------
local lastTurn = -1
local budget = 0
local turnFrames = 0
local seenUnitIds = {}		-- ids of our units seen earlier (stale-command source)
local stats = { actions = 0, illegal = 0, endturn = 0, popups = 0, uiq = 0, errors = 0 }

local function me() return Game.GetActivePlayer() end
local function P() return Players[me()] end

local function units(player)
	local t = {}
	for u in player:Units() do t[#t + 1] = u end
	return t
end
local function cities(player)
	local t = {}
	for c in player:Cities() do t[#t + 1] = c end
	return t
end

local function selectUnit(u) UI.ClearSelectionList(); UI.SelectUnit(u) end
local function pushMission(u, mission, x, y)
	selectUnit(u)
	Game.SelectionListGameNetMessage(GM.GAMEMESSAGE_PUSH_MISSION, mission, x or -1, y or -1, 0, false, false)
end
local function doCommand(u, cmd, d1, d2)
	selectUnit(u)
	Game.SelectionListGameNetMessage(GM.GAMEMESSAGE_DO_COMMAND, cmd, d1 or -1, d2 or -1, 0, false, false)
end

-- plots in a radius around (x, y) (Map.PlotXYWithRangeCheck is UI side; use Map.GetPlot with wrap)
local function plotsAround(x, y, r)
	local t = {}
	for dx = -r, r do
		for dy = -r, r do
			local p = Map.GetPlot(x + dx, y + dy)
			if p then t[#t + 1] = p end
		end
	end
	return t
end

-- ---- unit orders --------------------------------------------------------------------------------------------------------
local function orderSettler(player, u)
	local plot = u:GetPlot()
	if u:CanFound(plot) and (player:GetNumCities() == 0 or chance(0.6)) then
		pushMission(u, M("MISSION_FOUND"), u:GetX(), u:GetY())
		return "found"
	end
	local cands = {}
	for _, p in ipairs(plotsAround(u:GetX(), u:GetY(), 4)) do
		if player:CanFound(p:GetX(), p:GetY()) then cands[#cands + 1] = p end
	end
	local p = pick(cands)
	if p then pushMission(u, M("MISSION_MOVE_TO"), p:GetX(), p:GetY()); return "settle-move" end
	pushMission(u, M("MISSION_SKIP")); return "skip"
end

local function orderWorker(player, u)
	if chance(0.7) then doCommand(u, CT.COMMAND_AUTOMATE, AUTOMATE_BUILD); return "automate" end
	local builds = {}
	for b in GameInfo.Builds() do
		if u:CanStartMission(M("MISSION_BUILD"), b.ID, -1, u:GetPlot(), 0) then builds[#builds + 1] = b.ID end
	end
	local b = pick(builds)
	if b then pushMission(u, M("MISSION_BUILD"), b, -1); return "build" end
	pushMission(u, M("MISSION_SKIP")); return "skip"
end

local function orderTrade(player, u)
	local ok, dests = pcall(function() return player:GetPotentialInternationalTradeRouteDestinations(u) end)
	if ok and dests and #dests > 0 then
		local d = pick(dests)
		local plot = Map.GetPlot(d.plotX, d.plotY)
		if plot then
			pushMission(u, M("MISSION_ESTABLISH_TRADE_ROUTE"), plot:GetPlotIndex(), d.TradeConnectionType)
			return "trade-route"
		end
	end
	pushMission(u, M("MISSION_SKIP")); return "skip"
end

local function orderMilitary(player, u)
	-- promotions first (UnitPanel: COMMAND_PROMOTION)
	local proms = {}
	for pr in GameInfo.UnitPromotions() do
		if u:CanPromote(pr.ID, -1) then proms[#proms + 1] = pr.ID end
	end
	local pr = pick(proms)
	if pr then doCommand(u, CT.COMMAND_PROMOTION, pr); return "promote" end
	local r = rint(100)
	if r < 30 then
		-- attack or move into an adjacent/nearby hostile plot when there is one, else move somewhere
		local cands, hostile = {}, {}
		for _, p in ipairs(plotsAround(u:GetX(), u:GetY(), 2)) do
			if not (p:GetX() == u:GetX() and p:GetY() == u:GetY()) then
				cands[#cands + 1] = p
				if p:IsVisibleEnemyUnit(me()) or (p:IsCity() and Teams[player:GetTeam()]:IsAtWar(p:GetTeam())) then hostile[#hostile + 1] = p end
			end
		end
		local p = pick(#hostile > 0 and hostile or cands)
		if p then pushMission(u, M("MISSION_MOVE_TO"), p:GetX(), p:GetY()); return #hostile > 0 and "attack" or "move" end
	elseif r < 45 then
		doCommand(u, CT.COMMAND_AUTOMATE, AUTOMATE_EXPLORE); return "explore"
	elseif r < 75 then
		if u:CanStartMission(M("MISSION_FORTIFY"), -1, -1, u:GetPlot(), 0) then pushMission(u, M("MISSION_FORTIFY")); return "fortify" end
	elseif r < 85 then
		if u:CanStartMission(M("MISSION_HEAL"), -1, -1, u:GetPlot(), 0) then pushMission(u, M("MISSION_HEAL")); return "heal" end
	end
	pushMission(u, M("MISSION_SLEEP")); return "sleep"
end

local function orderGeneric(player, u)
	local missions = { "MISSION_DISCOVER", "MISSION_HURRY", "MISSION_TRADE", "MISSION_GREAT_WORK", "MISSION_GOLDEN_AGE",
		"MISSION_CULTURE_BOMB", "MISSION_SPREAD_RELIGION", "MISSION_FOUND_RELIGION", "MISSION_JOIN", "MISSION_CONSTRUCT",
		"MISSION_BUY_CITY_STATE", "MISSION_REPAIR_FLEET", "MISSION_SPACESHIP", "MISSION_ENHANCE_RELIGION" }
	local ok = {}
	for _, name in ipairs(missions) do
		local id = M(name)
		if id and u:CanStartMission(id, -1, -1, u:GetPlot(), 0) then ok[#ok + 1] = id end
	end
	local m = pick(ok)
	if m and chance(0.5) then pushMission(u, m); return "special" end
	return orderMilitary(player, u)
end

local function orderUnit(player, u)
	seenUnitIds[#seenUnitIds + 1] = u:GetID()
	if #seenUnitIds > 200 then table.remove(seenUnitIds, 1) end
	local info = GameInfo.Units[u:GetUnitType()]
	if info and info.Found then return orderSettler(player, u) end
	if u:WorkRate(true) > 0 then return orderWorker(player, u) end
	if u:IsTrade() then return orderTrade(player, u) end
	if u:IsCombatUnit() then return orderMilitary(player, u) end
	return orderGeneric(player, u)
end

local function readyUnits(player)
	local t = {}
	for _, u in ipairs(units(player)) do
		if u:IsReadyToMove() and not u:IsAutomated() then
			t[#t + 1] = u
		end
	end
	return t
end

-- ---- cities --------------------------------------------------------------------------------------------------------------
local function chooseProduction(player, c)
	local opts = {}
	for u in GameInfo.Units() do if c:CanTrain(u.ID) then opts[#opts + 1] = { OT.ORDER_TRAIN, u.ID } end end
	for b in GameInfo.Buildings() do if c:CanConstruct(b.ID) then opts[#opts + 1] = { OT.ORDER_CONSTRUCT, b.ID } end end
	for p in GameInfo.Projects() do if c:CanCreate(p.ID) then opts[#opts + 1] = { OT.ORDER_CREATE, p.ID } end end
	for p in GameInfo.Processes() do if c:CanMaintain(p.ID) then opts[#opts + 1] = { OT.ORDER_MAINTAIN, p.ID } end end
	-- plausible: settlers early, then defence/buildings
	local settler = GameInfo.Units.UNIT_SETTLER
	if settler and player:GetNumCities() < 4 and c:CanTrain(settler.ID) and chance(0.4) then
		Game.CityPushOrder(c, OT.ORDER_TRAIN, settler.ID, false, false, false); return "train-settler"
	end
	local o = pick(opts)
	if o then Game.CityPushOrder(c, o[1], o[2], false, chance(0.2), false); return "order" end
	return nil
end

local function cityAction(player, c)
	local r = rint(100)
	if c:GetOrderQueueLength() == 0 or r < 20 then return chooseProduction(player, c) end
	if r < 35 then
		-- purchase with gold when the UI would offer it
		local opts = {}
		for u in GameInfo.Units() do if c:IsCanPurchase(true, true, u.ID, -1, -1, YT.YIELD_GOLD) then opts[#opts + 1] = { "u", u.ID } end end
		for b in GameInfo.Buildings() do if c:IsCanPurchase(true, true, -1, b.ID, -1, YT.YIELD_GOLD) then opts[#opts + 1] = { "b", b.ID } end end
		local o = pick(opts)
		if o then
			if o[1] == "u" then Game.CityPurchaseUnit(c, o[2], YT.YIELD_GOLD) else Game.CityPurchaseBuilding(c, o[2], YT.YIELD_GOLD) end
			return "purchase"
		end
	elseif r < 45 then
		local foci = {}
		for _, k in ipairs({ "NO_CITY_AI_FOCUS_TYPE", "CITY_AI_FOCUS_TYPE_FOOD", "CITY_AI_FOCUS_TYPE_PRODUCTION", "CITY_AI_FOCUS_TYPE_GOLD",
			"CITY_AI_FOCUS_TYPE_SCIENCE", "CITY_AI_FOCUS_TYPE_CULTURE", "CITY_AI_FOCUS_TYPE_GREAT_PEOPLE", "CITY_AI_FOCUS_TYPE_FAITH" }) do
			if CityAIFocusTypes[k] then foci[#foci + 1] = CityAIFocusTypes[k] end
		end
		Network.SendSetCityAIFocus(c:GetID(), pick(foci) or -1); return "focus"
	elseif r < 50 then
		Network.SendSetCityAvoidGrowth(c:GetID(), chance(0.3)); return "avoid-growth"
	elseif r < 60 then
		local cands = {}
		for _, p in ipairs(plotsAround(c:GetX(), c:GetY(), 3)) do
			if c:CanBuyPlotAt(p:GetX(), p:GetY(), false) then cands[#cands + 1] = p end
		end
		local p = pick(cands)
		if p then Network.SendCityBuyPlot(c:GetID(), p:GetX(), p:GetY()); return "buy-plot" end
	elseif r < 70 then
		Network.SendUpdateCityCitizens(c:GetID()); return "update-citizens"
	end
	return nil
end

-- ---- empire-level choices ------------------------------------------------------------------------------------------------
local function chooseResearch(player, free)
	local opts = {}
	for t in GameInfo.Technologies() do if player:CanResearch(t.ID) then opts[#opts + 1] = t.ID end end
	local t = pick(opts)
	if t then Network.SendResearch(t, free and player:GetNumFreeTechs() or 0, -1, chance(0.2)); return "research" end
	return nil
end

local function choosePolicy(player)
	local opts = {}
	for b in GameInfo.PolicyBranchTypes() do if player:CanUnlockPolicyBranch(b.ID) then opts[#opts + 1] = { b.ID, false } end end
	for p in GameInfo.Policies() do if player:CanAdoptPolicy(p.ID) then opts[#opts + 1] = { p.ID, true } end end
	local o = pick(opts)
	if o then Network.SendUpdatePolicies(o[1], o[2], true); return "policy" end
	return nil
end

local function religionAction(player)
	if player:CanCreatePantheon(false) then
		local b = pick(Game.GetAvailablePantheonBeliefs() or {})
		if b then Network.SendFoundPantheon(me(), b); return "pantheon" end
	end
	return nil
end

local function foundReligion(player, x, y)
	local rels = {}
	for r in GameInfo.Religions() do
		if r.Type ~= "RELIGION_PANTHEON" and Game.GetHolyCityForReligion(r.ID, -1) == nil then rels[#rels + 1] = r.ID end	-- Game.GetFounder dereferences NULL for an unfounded religion (CvLuaGame.cpp lGetFounder)
	end
	local rel = pick(rels)
	local f = Game.GetAvailableFounderBeliefs() or {}
	local fo = Game.GetAvailableFollowerBeliefs() or {}
	if rel and #f > 0 and #fo > 0 then
		local cap = player:GetCapitalCity()
		x = x or (cap and cap:GetX()) or -1
		y = y or (cap and cap:GetY()) or -1
		Network.SendFoundReligion(me(), rel, "", pick(f), pick(fo), -1, -1, x, y)
		return "found-religion"
	end
	return nil
end

local function diploAction(player)
	local r = rint(100)
	local myTeam = Teams[player:GetTeam()]
	local minors, majors = {}, {}
	for i = 0, GameDefines.MAX_CIV_PLAYERS - 1 do
		local o = Players[i]
		if o and o:IsAlive() and i ~= me() and myTeam:IsHasMet(o:GetTeam()) then
			if o:IsMinorCiv() then minors[#minors + 1] = o else majors[#majors + 1] = o end
		end
	end
	local m = pick(minors)
	if r < 25 and m and player:GetGold() >= 250 then
		Game.DoMinorGiftGold(m:GetID(), pick({ 250, 500, 1000 })); return "cs-gift"
	elseif r < 40 and m and m:CanMajorBullyGold(me()) then
		Game.DoMinorBullyGold(me(), m:GetID()); return "cs-bully-gold"
	elseif r < 45 and m and m:CanMajorBullyUnit(me()) then
		Game.DoMinorBullyUnit(me(), m:GetID()); return "cs-bully-unit"
	elseif r < 55 and m and m:CanMajorStartProtection(me()) then
		Game.DoMinorPledgeProtection(me(), m:GetID(), true); return "cs-pledge"
	elseif r < 62 and m then
		local t = m:GetTeam()
		if myTeam:IsAtWar(t) then Network.SendChangeWar(t, false); return "cs-peace"
		elseif myTeam:CanDeclareWar(t) and chance(0.3) then Network.SendChangeWar(t, true); return "cs-war" end
	elseif r < 66 and m then
		Network.SendMinorNoUnitSpawning(m:GetID(), chance(0.5)); return "cs-spawning"
	elseif r < 70 then
		local o = pick(majors)
		if o and myTeam:CanDeclareWar(o:GetTeam()) and chance(0.2) then Network.SendChangeWar(o:GetTeam(), true); return "war" end
	end
	return nil
end

local function spyAction(player)
	local ok, spies = pcall(function() return player:GetEspionageSpies() end)
	if not ok or not spies or #spies == 0 then return nil end
	local spy = pick(spies)
	local targets = {}
	for i = 0, GameDefines.MAX_CIV_PLAYERS - 1 do
		local o = Players[i]
		if o and o:IsAlive() and i ~= me() then
			for c in o:Cities() do
				if c:IsRevealed(player:GetTeam(), false) then targets[#targets + 1] = { i, c:GetID() } end
			end
		end
	end
	local t = pick(targets)
	if t then Network.SendMoveSpy(me(), spy.AgentID, t[1], t[2], false); return "spy" end
	return nil
end

local function leagueAction(player)
	local league = Game.GetActiveLeague and Game.GetActiveLeague()
	if not league then return nil end
	if league:CanPropose(me()) then
		local opts = {}
		for res in GameInfo.Resolutions() do if league:CanProposeEnact(res.ID, me(), -1) then opts[#opts + 1] = res.ID end end
		local res = pick(opts)
		if res then Network.SendLeagueProposeEnact(league:GetID(), res, me(), -1); return "league-propose" end
	end
	if league:IsInSession() then
		local votes = league:GetRemainingVotesForMember(me())
		if votes > 0 then
			local props = league:GetEnactProposals() or {}
			local p = pick(props)
			if p and chance(0.8) then
				Network.SendLeagueVoteEnact(league:GetID(), p.ID, me(), votes, pick({ 0, 1 })); return "league-vote"
			end
			Network.SendLeagueVoteAbstain(league:GetID(), me(), votes); return "league-abstain"
		end
	end
	return nil
end

-- ---- popups (the UI answers what the DLL raised for this player) --------------------------------------------------------
local function handlePopup(player, pp)
	local t = pp.Type
	if t == BPT.BUTTONPOPUP_CHOOSE_GOODY_HUT_REWARD then
		local plot = Map.GetPlot(pp.Data1, pp.Data2)
		local u = player:GetUnitByID(pp.Data3)
		if plot and u then
			local goodies = {}
			for g in GameInfo.GoodyHuts() do if player:CanGetGoody(plot, g.ID, u) then goodies[#goodies + 1] = g.ID end end
			local g = pick(goodies)
			if g then Network.SendGoodyChoice(me(), plot:GetX(), plot:GetY(), g, u:GetID()); return "goody" end
		end
	elseif t == BPT.BUTTONPOPUP_CHOOSE_IDEOLOGY then
		local opts = {}
		for _, k in ipairs({ "POLICY_BRANCH_FREEDOM", "POLICY_BRANCH_ORDER", "POLICY_BRANCH_AUTOCRACY" }) do
			if GameInfo.PolicyBranchTypes[k] then opts[#opts + 1] = GameInfo.PolicyBranchTypes[k].ID end
		end
		local b = pick(opts)
		if b then Network.SendIdeologyChoice(me(), b); return "ideology" end
	elseif t == BPT.BUTTONPOPUP_FOUND_RELIGION then
		return foundReligion(player, nil, nil)
	elseif t == BPT.BUTTONPOPUP_FOUND_PANTHEON then
		return religionAction(player)
	elseif t == BPT.BUTTONPOPUP_CHOOSE_FREE_GREAT_PERSON or t == BPT.BUTTONPOPUP_CHOOSE_FAITH_GREAT_PERSON or t == BPT.BUTTONPOPUP_CHOOSE_MAYA_BONUS then
		local gps = {}
		for u in GameInfo.Units() do if u.Special == "SPECIALUNIT_PEOPLE" then gps[#gps + 1] = u.ID end end
		local u = pick(gps)
		if u then
			if t == BPT.BUTTONPOPUP_CHOOSE_FREE_GREAT_PERSON then Network.SendGreatPersonChoice(me(), u)
			elseif t == BPT.BUTTONPOPUP_CHOOSE_FAITH_GREAT_PERSON then Network.SendFaithGreatPersonChoice(me(), u)
			else Network.SendMayaBonusChoice(me(), u) end
			return "great-person"
		end
	elseif t == BPT.BUTTONPOPUP_CHOOSE_ARCHAEOLOGY then
		Network.SendArchaeologyChoice(me(), pp.Data1, 1 + rint(3)); return "archaeology"
	elseif t == BPT.BUTTONPOPUP_CHOOSETECH then
		return chooseResearch(player, false)
	elseif t == BPT.BUTTONPOPUP_CHOOSEPOLICY then
		return choosePolicy(player)
	elseif t == BPT.BUTTONPOPUP_CHOOSEPRODUCTION then
		local c = player:GetCityByID(pp.Data1)
		if c then return chooseProduction(player, c) end
	end
	return nil
end

-- ---- illegal / stale commands ----------------------------------------------------------------------------------------------
local function illegalAction(player)
	local r = rint(6)
	if r == 0 then
		-- two orders for one unit in the same frame: the second targets a state the first one changes (found, then move;
		-- a stale target once the first executed). Commands for units that die before delivery come with --remote-delay.
		local u = pick(units(player))
		if u then
			pushMission(u, M("MISSION_FOUND"), u:GetX(), u:GetY())
			pushMission(u, M("MISSION_MOVE_TO"), u:GetX() + 1, u:GetY())
			return "double-order"
		end
	elseif r == 1 then
		-- ids stay inside the table (unavailable / known techs): an id past the end crashes ResponseResearch -> CanEverResearch
		-- (v-release too; campaign A finding), which would end every long run; HarnessBotConfig.oob_ids = true restores 0..199
		local n = 0
		for _ in GameInfo.Technologies() do n = n + 1 end
		Network.SendResearch(cfg.oob_ids and rint(200) or rint(n), 0, -1, false); return "bad-research"
	elseif r == 2 then
		local n = 0
		for _ in GameInfo.Policies() do n = n + 1 end
		Network.SendUpdatePolicies(cfg.oob_ids and rint(150) or rint(n), true, true); return "bad-policy"
	elseif r == 3 then
		local c = pick(cities(player))
		if c then Network.SendCityBuyPlot(c:GetID(), rint(80), rint(80)); return "bad-buy-plot" end
	elseif r == 4 then
		Network.SendDoTask(rint(10000), rint(10), rint(5), rint(5), false, false, false, false); return "bad-task"
	elseif r == 5 then
		local u = pick(units(player))
		-- the target stays on the map: an off-map MOVE_TO is accepted by ResponsePushMission and later crashes the owner's
		-- machine in CvUnit::setExperience (null plot, active-player branch; campaign A finding); HarnessBotConfig.oob_ids
		-- = true restores the off-map target
		if u then
			local w, h = Map.GetGridSize()
			local x, y = u:GetX() + 30, u:GetY() + 30
			if not cfg.oob_ids then x = x % w; y = math.min(y, h - 1) end
			pushMission(u, M("MISSION_MOVE_TO"), x, y); return "far-move"
		end
	end
	return nil
end

-- ---- UI queries (read only; what the local screens call; DS-0034 shows such a "read" can write) ---------------------------
local function UiQueries(player)
	local n = 0
	local function q(f) local ok = pcall(f); n = n + 1; if not ok then stats.errors = stats.errors + 1 end end
	local which = rint(5)
	for i = 0, GameDefines.MAX_CIV_PLAYERS - 1 do
		local o = Players[i]
		if o and o:IsEverAlive() then
			if which == 0 then	-- Demographics / WhosWinning
				q(function() return o:GetMilitaryMight() end)
				q(function() return o:GetTotalPopulation(), o:CalculateGrossGold(), o:GetNumPlots() end)
			elseif which == 1 then	-- diplomacy overview / leader list
				q(function() return o:GetScore(), o:GetGold(), o:GetTotalJONSCulturePerTurn() end)
				q(function() return Teams[o:GetTeam()]:GetScore() end)
			elseif which == 2 then	-- victory progress
				q(function() return o:GetScore(), Teams[o:GetTeam()]:GetTechCount() end)
			end
		end
	end
	if which == 3 then	-- city view / tooltips of own cities
		for _, c in ipairs(cities(player)) do
			q(function() return c:GetYieldRate(YT.YIELD_FOOD), c:GetFoodTurnsLeft(), c:GetProductionTurnsLeft(), c:GetJONSCulturePerTurn() end)
			q(function() return c:GetBuyPlotCost(c:GetX(), c:GetY()), c:GetStrengthValue() end)
		end
	elseif which == 4 then	-- tech tree
		for t in GameInfo.Technologies() do q(function() return player:GetResearchTurnsLeft(t.ID, true), player:CanResearch(t.ID) end) end
	end
	stats.uiq = stats.uiq + n
	return n
end

-- ---- end of turn -----------------------------------------------------------------------------------------------------------
local function tryEndTurn(player)
	local block = player:GetEndTurnBlockingType()
	if block == ETB.ENDTURN_BLOCKING_RESEARCH or block == ETB.ENDTURN_BLOCKING_FREE_TECH then return chooseResearch(player, block == ETB.ENDTURN_BLOCKING_FREE_TECH) end
	if block == ETB.ENDTURN_BLOCKING_POLICY or block == ETB.ENDTURN_BLOCKING_FREE_POLICY then return choosePolicy(player) end
	if block == ETB.ENDTURN_BLOCKING_PRODUCTION then
		for _, c in ipairs(cities(player)) do if c:GetOrderQueueLength() == 0 then return chooseProduction(player, c) end end
	end
	if block == ETB.ENDTURN_BLOCKING_FOUND_PANTHEON then return religionAction(player) end
	if block == ETB.ENDTURN_BLOCKING_FOUND_RELIGION then return foundReligion(player, nil, nil) end
	if block == ETB.ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS or block == ETB.ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES then return leagueAction(player) end
	if block == ETB.ENDTURN_BLOCKING_UNIT_PROMOTION or block == ETB.ENDTURN_BLOCKING_UNITS or block == ETB.ENDTURN_BLOCKING_UNIT_NEEDS_ORDERS
		or block == ETB.ENDTURN_BLOCKING_STACKED_UNITS then
		local u = pick(readyUnits(player))
		if u then return orderUnit(player, u) end
	end
	if block == nil or block == ETB.NO_ENDTURN_BLOCKING_TYPE or block == -1 then
		Game.DoControl(CONTROL.CONTROL_ENDTURN); stats.endturn = stats.endturn + 1
		return "end-turn"
	end
	if turnFrames > stuckFrames / 2 and (block == ETB.ENDTURN_BLOCKING_UNITS or block == ETB.ENDTURN_BLOCKING_STACKED_UNITS) then
		Game.DoControl(CONTROL.CONTROL_FORCEENDTURN); return "force-end-turn"
	end
	return "blocked-" .. tostring(block)
end

-- ---- trade screen with AI majors (HarnessBotConfig.trade > 0) ------------------------------------------------------------
-- The same calls the trade screen makes (LEKMOD\Lua\tmp\ui\LeaderHead\TradeLogic.lua.ignore and the EUI variant;
-- LeaderHeadRoot / DiscussionDialog for the buttons): UI.GetScratchDeal() is the table, Deal:Add* the pockets,
-- UI.DoProposeDeal / UI.DoEqualizeDealWithHuman / UI.DoWhatDoesAIWant / UI.DoWhatWillAIGive the buttons,
-- Player:DoTradeScreenOpened / DoTradeScreenClosed(bAIMakingOffer) and UI.RequestLeaveLeader the screen's open/close,
-- Game.DoFromUIDiploEvent the replies (peace request, demand / request refusal, discussion answers). The AI answers with
-- leader messages (civhost queues them as popups of type 10000: Data1 = AI player, Data2 = DiploUIStateTypes, Data3 = data).
-- Everything here runs on this peer only, like the real screen; the other machines see only what the DLL sends
-- (NetDealAccepted, NetDemandAccepted, FromUIDiploEvent). With trade = 0 nothing below runs and no PRNG value is drawn.
local tradeShare = tonumber(cfg.trade) or 0
-- per trade-screen step, chance to retype the amount boxes (TradeLogic ChangeGoldPerTurnAmount / ChangeGoldAmount:
-- Deal:ChangeGoldPerTurnTrade / ChangeGoldTrade on items already on the table); 0 = off, no PRNG value drawn
local tradeEdit = tonumber(cfg.trade_edit) or 0
local LEADER_MESSAGE = 10000
local TI = TradeableItems or {}
local DUI = DiploUIStateTypes or {}
local FUE = FromUIDiploEventTypes or {}
local tradeScreen = nil		-- { them, aiOffer, frames, proposed, state }
local tstats = { opened = 0, proposed = 0, equalize = 0, aioffer = 0, accept = 0, decline = 0, demand = 0, peace = 0, discuss = 0, closed = 0,
	ask = 0, msgs = 0, edit = 0 }

local function dealDuration() local ok, d = pcall(function() return Game.GetDealDuration() end); return (ok and d) or 30 end

local function metAIMajors(player)
	local t = {}
	local myTeam = Teams[player:GetTeam()]
	for i = 0, GameDefines.MAX_MAJOR_CIVS - 1 do
		local o = Players[i]
		if o and o:IsAlive() and i ~= me() and not o:IsHuman() and not o:IsMinorCiv() and myTeam:IsHasMet(o:GetTeam()) then t[#t + 1] = i end
	end
	return t
end

-- 1-3 items a human could put on the table (TradeLogic pockets), each checked with Deal:IsPossibleToTradeItem
local function fillTable(d, us, them)
	local dur = dealDuration()
	local opts = {}
	local function res(from, to)
		for r in GameInfo.Resources() do
			local usage = r.ResourceUsage
			if usage == 2 or usage == 1 then
				local n = (usage == 2) and 1 or math.max(1, math.min(Players[from]:GetNumResourceAvailable(r.ID, false), 1 + rint(3)))
				if d:IsPossibleToTradeItem(from, to, TI.TRADE_ITEM_RESOURCES, r.ID, n) then
					opts[#opts + 1] = function() d:AddResourceTrade(from, r.ID, n, dur) end
				end
			end
		end
	end
	res(us, them); res(them, us)
	for _, pair in ipairs({ { us, them }, { them, us } }) do
		local from, to = pair[1], pair[2]
		local gold = d:GetGoldAvailable(from, -1)
		if gold and gold >= 10 and d:IsPossibleToTradeItem(from, to, TI.TRADE_ITEM_GOLD, 1) then
			opts[#opts + 1] = function() d:AddGoldTrade(from, 10 * (1 + rint(math.max(1, math.floor(gold / 10))))) end
		end
		local gpt = 1 + rint(8)
		if d:IsPossibleToTradeItem(from, to, TI.TRADE_ITEM_GOLD_PER_TURN, gpt, dur) then
			opts[#opts + 1] = function() d:AddGoldPerTurnTrade(from, gpt, dur) end
		end
		if d:IsPossibleToTradeItem(from, to, TI.TRADE_ITEM_OPEN_BORDERS, dur) then
			opts[#opts + 1] = function() d:AddOpenBorders(from, dur) end
		end
		if d:IsPossibleToTradeItem(from, to, TI.TRADE_ITEM_ALLOW_EMBASSY, dur) then
			opts[#opts + 1] = function() d:AddAllowEmbassy(from) end
		end
	end
	if d:IsPossibleToTradeItem(us, them, TI.TRADE_ITEM_RESEARCH_AGREEMENT, dur) and d:IsPossibleToTradeItem(them, us, TI.TRADE_ITEM_RESEARCH_AGREEMENT, dur) then
		opts[#opts + 1] = function() d:AddResearchAgreement(us, dur); d:AddResearchAgreement(them, dur) end
	end
	local n = 1 + rint(3)
	for _ = 1, n do
		local f = pick(opts)
		if f then pcall(f) end
	end
	return d:GetNumItems()
end

-- the amount boxes: a new value for the gold-per-turn / lump-gold item of one side, clamped the way TradeLogic clamps it
-- (CalculateGoldRate / GetGoldAvailable); the Change* calls do nothing when that side has no such item on the table
local function editAmounts(d, us, them)
	local from = chance(0.5) and us or them
	local rate = Players[from]:CalculateGoldRate()
	d:ChangeGoldPerTurnTrade(from, math.min(1 + rint(12), rate), dealDuration())
	local gold = d:GetGoldAvailable(from, TI.TRADE_ITEM_GOLD)
	if gold and gold > 0 then d:ChangeGoldTrade(from, math.min(gold, 10 * (1 + rint(10)))) end
	tstats.edit = tstats.edit + 1
	return "trade-edit"
end

local function closeTradeScreen()
	if not tradeScreen then return nil end
	local them = tradeScreen.them
	if Players[them] then Players[them]:DoTradeScreenClosed(tradeScreen.aiOffer and true or false) end
	UI.RequestLeaveLeader()
	tradeScreen = nil
	tstats.closed = tstats.closed + 1
	return "trade-close"
end

local function proposeTable(us, them)
	local d = UI.GetScratchDeal()
	if not d then return nil end
	d:SetFromPlayer(us); d:SetToPlayer(them)
	UI.DoProposeDeal()
	tstats.proposed = tstats.proposed + 1
	return "trade-propose"
end

-- open the trade screen with an AI major (LeaderHeadRoot: greeting, then Trade; at war the Trade button asks for peace)
local function openTradeScreen(player)
	local them = pick(metAIMajors(player))
	if not them then return nil end
	local us = me()
	Players[them]:DoBeginDiploWithHuman()
	tradeScreen = { them = them, aiOffer = false, frames = 0, proposed = 0 }
	tstats.opened = tstats.opened + 1
	if Teams[player:GetTeam()]:IsAtWar(Players[them]:GetTeam()) then
		-- LeaderHeadRoot OnTrade at war: FROM_UI_DIPLO_EVENT_HUMAN_NEGOTIATE_PEACE; the AI answers (DIPLO_UI_STATE_TRADE with the
		-- scratch deal set up by DoTradeScreenOpened on this machine) when the message comes back. Sometimes also the direct call.
		Game.DoFromUIDiploEvent(FUE.FROM_UI_DIPLO_EVENT_HUMAN_NEGOTIATE_PEACE, them, 0, 0)
		if chance(0.3) then Players[them]:DoTradeScreenOpened() end
		tradeScreen.peace = true
		tstats.peace = tstats.peace + 1
		return "trade-open-peace"
	end
	Players[them]:DoTradeScreenOpened()		-- UI.OnHumanOpenedTradeScreen path; no-op when not at war
	local d = UI.GetScratchDeal()
	if not d then tradeScreen = nil; return nil end
	d:ClearItems(); d:SetFromPlayer(us); d:SetToPlayer(them)
	fillTable(d, us, them)
	return "trade-open"
end

-- one step of an open screen (a human clicking around for a few frames)
local function tradeScreenStep(player)
	local t = tradeScreen
	t.frames = t.frames + 1
	local us, them = me(), t.them
	if not Players[them] or not Players[them]:IsAlive() then return closeTradeScreen() end
	if t.frames > 6 or (t.proposed >= 2) or chance(0.15) then return closeTradeScreen() end
	if t.peace then
		-- the peace table is on the scratch deal once the AI answered; sometimes sweeten it, then propose
		if t.state == DUI.DIPLO_UI_STATE_TRADE or t.frames >= 3 then
			local d = UI.GetScratchDeal()
			if d and d:GetNumItems() > 0 then
				if chance(0.3) then
					local gold = d:GetGoldAvailable(us, -1)
					if gold and gold >= 20 then pcall(function() d:AddGoldTrade(us, 10 * (1 + rint(math.floor(gold / 20)))) end) end
				end
				t.proposed = t.proposed + 1
				return proposeTable(us, them)
			end
		end
		return "trade-wait"
	end
	local d = UI.GetScratchDeal()
	if not d then return closeTradeScreen() end
	if tradeEdit > 0 and d:GetNumItems() > 0 and chance(tradeEdit) then return editAmounts(d, us, them) end
	local r = rint(100)
	if d:GetNumItems() == 0 or r < 15 then
		d:ClearItems(); d:SetFromPlayer(us); d:SetToPlayer(them)
		fillTable(d, us, them)
		return "trade-fill"
	elseif r < 35 then
		tstats.equalize = tstats.equalize + 1
		local which = rint(3)
		if which == 0 then UI.DoEqualizeDealWithHuman() elseif which == 1 then UI.DoWhatDoesAIWant() else UI.DoWhatWillAIGive() end
		return "trade-equalize"
	end
	t.proposed = t.proposed + 1
	return proposeTable(us, them)
end

-- DiscussionDialog buttons: state -> reply event (button 1 or 2; Data3 is the extra data the dialog passes back)
local discussReplies = nil
local function discussReply(state)
	if not discussReplies then
		discussReplies = {}
		local function m(s, e) if DUI[s] and FUE[e] then discussReplies[DUI[s]] = FUE[e] end end
		m("DIPLO_UI_STATE_DISCUSS_AGGRESSIVE_MILITARY_WARNING", "FROM_UI_DIPLO_EVENT_AGGRESSIVE_MILITARY_WARNING_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_I_ATTACKED_YOUR_MINOR_CIV", "FROM_UI_DIPLO_EVENT_I_ATTACKED_YOUR_MINOR_CIV_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_I_BULLIED_YOUR_MINOR_CIV", "FROM_UI_DIPLO_EVENT_I_BULLIED_YOUR_MINOR_CIV_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_YOU_ATTACKED_MINOR_CIV", "FROM_UI_DIPLO_EVENT_ATTACKED_MINOR_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_YOU_KILLED_MINOR_CIV", "FROM_UI_DIPLO_EVENT_KILLED_MINOR_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_YOU_BULLIED_MINOR_CIV", "FROM_UI_DIPLO_EVENT_BULLIED_MINOR_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_YOU_EXPANSION_SERIOUS_WARNING", "FROM_UI_DIPLO_EVENT_EXPANSION_SERIOUS_WARNING_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_YOU_EXPANSION_WARNING", "FROM_UI_DIPLO_EVENT_EXPANSION_WARNING_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_YOU_PLOT_BUYING_SERIOUS_WARNING", "FROM_UI_DIPLO_EVENT_PLOT_BUYING_SERIOUS_WARNING_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_YOU_PLOT_BUYING_WARNING", "FROM_UI_DIPLO_EVENT_PLOT_BUYING_WARNING_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_WORK_WITH_US", "FROM_UI_DIPLO_EVENT_WORK_WITH_US_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_COOP_WAR", "FROM_UI_DIPLO_EVENT_COOP_WAR_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_COOP_WAR_TIME", "FROM_UI_DIPLO_EVENT_COOP_WAR_NOW_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_PLAN_RESEARCH_AGREEMENT", "FROM_UI_DIPLO_EVENT_PLAN_RA_RESPONSE")
		m("DIPLO_UI_STATE_DISCUSS_AI_REQUEST_DENOUNCE", "FROM_UI_DIPLO_EVENT_AI_REQUEST_DENOUNCE_RESPONSE")
		m("DIPLO_UI_STATE_CAUGHT_YOUR_SPY", "FROM_UI_DIPLO_EVENT_CAUGHT_YOUR_SPY_RESPONSE")
		m("DIPLO_UI_STATE_KILLED_MY_SPY", "FROM_UI_DIPLO_EVENT_KILLED_MY_SPY_RESPONSE")
		m("DIPLO_UI_STATE_STOP_CONVERSIONS", "FROM_UI_DIPLO_EVENT_STOP_CONVERSIONS")
		m("DIPLO_UI_STATE_STOP_DIGGING", "FROM_UI_DIPLO_EVENT_STOP_DIGGING")
	end
	return discussReplies[state]
end

-- a leader message from an AI to this human (popup type LEADER_MESSAGE)
local function leaderMessage(player, pp)
	tstats.msgs = tstats.msgs + 1
	local them, state, data = pp.Data1, pp.Data2, pp.Data3
	local us = me()
	if tradeScreen and tradeScreen.them ~= them and (state == DUI.DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER or
		state == DUI.DIPLO_UI_STATE_TRADE_AI_MAKES_DEMAND or state == DUI.DIPLO_UI_STATE_TRADE_AI_MAKES_REQUEST) then
		closeTradeScreen()		-- the other leader's screen closes before this one opens
	end
	if state == DUI.DIPLO_UI_STATE_TRADE_AI_MAKES_OFFER then
		-- AI offer, renewal, research agreement or peace offer (CvDiplomacyRequests::SendDealRequest filled the scratch deal)
		tstats.aioffer = tstats.aioffer + 1
		tradeScreen = { them = them, aiOffer = true, frames = 4, proposed = 1 }
		if chance(0.55) then
			tstats.accept = tstats.accept + 1
			return proposeTable(us, them) and "trade-ai-offer-accept"
		end
		tstats.decline = tstats.decline + 1
		return closeTradeScreen() and "trade-ai-offer-decline"
	elseif state == DUI.DIPLO_UI_STATE_TRADE_AI_MAKES_DEMAND or state == DUI.DIPLO_UI_STATE_TRADE_AI_MAKES_REQUEST then
		tstats.demand = tstats.demand + 1
		tradeScreen = { them = them, aiOffer = false, frames = 4, proposed = 1 }
		if chance(0.4) then
			return proposeTable(us, them) and "trade-ai-demand-accept"
		end
		if state == DUI.DIPLO_UI_STATE_TRADE_AI_MAKES_DEMAND then
			Game.DoFromUIDiploEvent(FUE.FROM_UI_DIPLO_EVENT_DEMAND_HUMAN_REFUSAL, them, 0, 0)
		else
			local d = UI.GetScratchDeal()
			local v = 0
			if d then local ok, x = pcall(function() return player:GetDealMyValue(d) end); v = (ok and x) or 0 end
			Game.DoFromUIDiploEvent(FUE.FROM_UI_DIPLO_EVENT_REQUEST_HUMAN_REFUSAL, them, v, 0)
		end
		return "trade-refuse-demand"
	elseif state == DUI.DIPLO_UI_STATE_TRADE then
		if tradeScreen and tradeScreen.them == them then tradeScreen.state = state end
		return nil
	end
	local ev = discussReply(state)
	if ev then
		tstats.discuss = tstats.discuss + 1
		local button = 1 + rint(2)
		Game.DoFromUIDiploEvent(ev, them, button, data or -1)
		UI.RequestLeaveLeader()
		return "discuss-reply"
	end
	return nil
end

-- human-invoked discussion requests (DiscussionDialog HUMAN_INVOKED buttons), occasionally
local function askAI(player)
	local them = pick(metAIMajors(player))
	if not them or Teams[player:GetTeam()]:IsAtWar(Players[them]:GetTeam()) then return nil end
	local evs = { "FROM_UI_DIPLO_EVENT_HUMAN_DISCUSSION_STOP_SPREADING_RELIGION", "FROM_UI_DIPLO_EVENT_HUMAN_DISCUSSION_STOP_DIGGING",
		"FROM_UI_DIPLO_EVENT_HUMAN_DISCUSSION_DONT_SETTLE", "FROM_UI_DIPLO_EVENT_HUMAN_DISCUSSION_WORK_WITH_US",
		"FROM_UI_DIPLO_EVENT_HUMAN_DISCUSSION_STOP_SPYING" }
	local e = FUE[pick(evs)]
	if not e then return nil end
	Players[them]:DoBeginDiploWithHuman()
	Game.DoFromUIDiploEvent(e, them, 0, 0)
	UI.RequestLeaveLeader()
	tstats.ask = tstats.ask + 1
	return "discuss-ask"
end

-- ---- tick ------------------------------------------------------------------------------------------------------------------
local actions = { cityAction, diploAction, spyAction, leagueAction, religionAction }

local function act(f, ...)
	local ok, res = pcall(f, ...)
	if not ok then stats.errors = stats.errors + 1; note("err:" .. tostring(res):sub(1, 80)); return nil end
	if res then stats.actions = stats.actions + 1; note(res) end
	return res
end

function HarnessBotTick(frame, mode)
	log = {}
	local player = P()
	if not player or not player:IsAlive() then return "" end
	for _, pp in ipairs(HstUiTakePopups()) do
		stats.popups = stats.popups + 1
		if pp.Type == LEADER_MESSAGE then
			if tradeShare > 0 then act(leaderMessage, player, pp) end
		else
			act(handlePopup, player, pp)
		end
	end
	if uiQueries and chance(0.05) then act(UiQueries, player) end
	local turn = Game.GetGameTurn()
	if turn ~= lastTurn then
		lastTurn = turn
		turnFrames = 0
		budget = math.floor(density * (player:GetNumUnits() + player:GetNumCities() + 3) + 0.5)
	end
	turnFrames = turnFrames + 1
	if tradeShare > 0 and tradeScreen then
		-- a human in the diplomacy screen does nothing else; after the turn ended the screen is just closed
		if not player:IsTurnActive() or Network.HasSentNetTurnComplete() then act(closeTradeScreen) else act(tradeScreenStep, player) end
		return table.concat(log, ",")
	end
	if not player:IsTurnActive() or Network.HasSentNetTurnComplete() then return table.concat(log, ",") end
	if tradeShare > 0 and chance(tradeShare) then
		if chance(0.15) then act(askAI, player) else act(openTradeScreen, player) end
		return table.concat(log, ",")
	end
	-- at most two commands per frame, like a quick human
	for _ = 1, 2 do
		if chance(illegalShare) then
			stats.illegal = stats.illegal + 1
			act(illegalAction, player)
		elseif budget > 0 then
			budget = budget - 1
			local ready = readyUnits(player)
			if #ready > 0 and chance(0.6) then
				act(orderUnit, player, pick(ready))
			elseif player:GetCurrentResearch() == -1 and chance(0.5) then
				act(chooseResearch, player, false)
			elseif chance(0.2) then
				act(choosePolicy, player)
			else
				local f = pick(actions)
				if f == cityAction then
					local c = pick(cities(player))
					if c then act(cityAction, player, c) end
				else
					act(f, player)
				end
			end
		else
			act(tryEndTurn, player)
			break
		end
	end
	return table.concat(log, ",")
end

function HarnessBotStats()
	local s = string.format("actions=%d illegal=%d endturn=%d popups=%d uiq=%d errors=%d", stats.actions, stats.illegal, stats.endturn,
		stats.popups, stats.uiq, stats.errors)
	if tradeShare > 0 then
		s = s .. string.format(" trade: opened=%d proposed=%d equalize=%d aioffer=%d accept=%d decline=%d demand=%d peace=%d discuss=%d ask=%d closed=%d msgs=%d",
			tstats.opened, tstats.proposed, tstats.equalize, tstats.aioffer, tstats.accept, tstats.decline, tstats.demand, tstats.peace,
			tstats.discuss, tstats.ask, tstats.closed, tstats.msgs)
		if tradeEdit > 0 then s = s .. string.format(" edit=%d", tstats.edit) end
	end
	return s
end
