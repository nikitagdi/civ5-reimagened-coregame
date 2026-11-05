-- Author: EnormousApplePie

------------------------------------------------------------------------------------------------------------------------
-- FIX-0005 / FIX-0006: synchronous balance bugs, awaiting the user's approval.
-- They are OFF (= original behaviour, byte-for-byte same decisions) until the user sets the switch to true:
--   FIX_0005: `ipairs(Players)` starts at index 1, so player 0 (the host slot) never got dummy buildings / dummy policies.
--   FIX_0006: `player == player_or_team_id` compares a player object with a number (always false), so only the
--             `player:GetTeam() == id` branch ever matched: for player events (PlayerAdoptPolicy, PlayerCityFounded, ...)
--             that is "the player whose TEAM id equals the player id", wrong in team games.
local FIX_0005_INCLUDE_PLAYER_0 = false
local FIX_0006_MATCH_PLAYER_ID = false
-- true only while a TEAM event (TeamSetHasTech passes a team id) is handled; used by FIX_0006
local lekmod_dummy_event_is_team = false

-- Players to visit. Original: ipairs(Players). FIX-0005: 0 .. MAX_MAJOR_CIVS-1 (the loops only act on major civs anyway)
local function lekmod_dummy_players()
   if not FIX_0005_INCLUDE_PLAYER_0 then return ipairs(Players) end
   local list = {}
   for i = 0, GameDefines.MAX_MAJOR_CIVS - 1, 1 do
      list[#list + 1] = Players[i]
   end
   return ipairs(list)
end

------------------------------------------------------------------------------------------------------------------------
-- Global dummy buildings. Adds a permanent dummy building to every city (or just the capital),
-- Supports various checks. Data set in the xml (XML Overrides/civ5Units.xml)
------------------------------------------------------------------------------------------------------------------------
function lekmod_add_global_dummy_buildings(player_or_team_id)

   local valid_buildings = {}

   if GameInfo.Global_Dummy_Buildings == nil then print("dummy table does not exist, check the xml!") return end
   for dummy_data in GameInfo.Global_Dummy_Buildings() do

      local civilization_type = dummy_data.CivilizationType
      local is_capital_only = dummy_data.IsCapitalOnly
      local building_type = dummy_data.BuildingType
      local technology_type = dummy_data.TechType
      local is_golden_age = dummy_data.RequiresGoldenAge
      local policy_type = dummy_data.PolicyType
      local policy_branch_finished = dummy_data.PolicyBranchFinished
      local policy_branch_chosen = dummy_data.PolicyBranchChosen

      -- FIX-0005: was `for _, player in ipairs(Players) do` (skips player 0)
      for _, player in lekmod_dummy_players() do

         -- FIX-0006: original match is `(player == player_or_team_id or player:GetTeam() == player_or_team_id)`
         local event_matches_player
         if FIX_0006_MATCH_PLAYER_ID then
            if lekmod_dummy_event_is_team then
               event_matches_player = (player:GetTeam() == player_or_team_id)
            else
               event_matches_player = (player:GetID() == player_or_team_id)
            end
         else
            event_matches_player = (player == player_or_team_id or player:GetTeam() == player_or_team_id)
         end

         if player:IsAlive() and not player:IsBarbarian() and not player:IsMinorCiv() and
         event_matches_player then

            -- first delete all instances of the building, then check if we can re-add it anywhere
            for city in player:Cities() do
               if city then
                  city:SetNumRealBuilding(GameInfoTypes[building_type], 0)
               end
            end
            if civilization_type ~= nil and player:GetCivilizationType() ~= GameInfoTypes[civilization_type] then -- skip
            elseif technology_type ~= nil
            and (not Teams[player:GetTeam()]:IsHasTech(GameInfoTypes[technology_type])) then -- skip
            elseif is_golden_age and (not player:IsGoldenAge()) then -- skip
            elseif policy_type ~= nil and not player:HasPolicy(GameInfoTypes[policy_type]) then -- skip
            elseif policy_branch_finished ~= nil
            and not player:IsPolicyBranchFinished(GameInfoTypes[policy_branch_finished]) then -- skip
            elseif policy_branch_chosen ~= nil
            -- Note: HasPolicyBranch is a Lekmod method. It is not available in the base game
            and not player:HasPolicyBranch(GameInfoTypes[policy_branch_chosen]) then -- skip
            else
               for city in player:Cities() do
                  if city then
                     if is_capital_only and not city:IsCapital() then -- skip
                     else
                        valid_buildings[player:GetID()] = valid_buildings[player:GetID()] or {}
                        -- Collect all building we can add, then add them later.
                        -- This is to avoid potential conflicts with multiple building entries of the same type.
                        print("Adding building: " .. building_type .. " to player: " .. player:GetID())
                        table.insert(valid_buildings[player:GetID()], {building_type, is_capital_only})
                     end
                  end
               end
            end
         end

      end

   end

   -- Add the buildings
   for player_id, buildings in pairs(valid_buildings) do
      for _, building_data in ipairs(buildings) do
         local building_type = building_data[1]
         local is_capital_only = building_data[2]
         for city in Players[player_id]:Cities() do
            if city then
               if is_capital_only and not city:IsCapital() then -- skip
               else
                  city:SetNumRealBuilding(GameInfoTypes[building_type], 1)
               end
            end
         end
      end
   end

   -- Reset the valid buildings table
   for player_id, _ in pairs(valid_buildings) do
      valid_buildings[player_id] = nil
   end

end
-- FIX-0006: TeamSetHasTech passes a TEAM id (all the other events pass a player id)
function lekmod_add_global_dummy_buildings_on_team_tech(team_id)

   lekmod_dummy_event_is_team = true
   lekmod_add_global_dummy_buildings(team_id)
   lekmod_dummy_event_is_team = false

end
function lekmod_add_global_dummy_buildings_on_capture(old_owner_id, _,_,_, new_owner_id)

   lekmod_add_global_dummy_buildings(old_owner_id)
   lekmod_add_global_dummy_buildings(new_owner_id)

end
GameEvents.PlayerCityFounded.Add(lekmod_add_global_dummy_buildings)
GameEvents.CityCaptureComplete.Add(lekmod_add_global_dummy_buildings_on_capture)
-- FIX-0006: with the switch off this is the original registration; on, the team-aware wrapper
if FIX_0006_MATCH_PLAYER_ID then
   GameEvents.TeamSetHasTech.Add(lekmod_add_global_dummy_buildings_on_team_tech)
else
   GameEvents.TeamSetHasTech.Add(lekmod_add_global_dummy_buildings)
end
GameEvents.PlayerAdoptPolicy.Add(lekmod_add_global_dummy_buildings)
-- Note: PlayerPolicyBranchFinished is a Lekmod Event! Not available in the base game
GameEvents.PlayerPolicyBranchUnlocked.Add(lekmod_add_global_dummy_buildings)
-- Note: PlayerGoldenAge is a Lekmod Event! Not available in the base game
GameEvents.PlayerSetGoldenAge.Add(lekmod_add_global_dummy_buildings)
------------------------------------------------------------------------------------------------------------------------
-- Global dummy policies. Set at the start of the game. Data set in the xml (XML Overrides/civ5Units.xml)
------------------------------------------------------------------------------------------------------------------------
function lekmod_add_dummy_policies()

   if GameInfo.Civilization_Dummy_Policies == nil then print("dummy table does not exist, check the xml!") return end
   for dummy_data in GameInfo.Civilization_Dummy_Policies() do

      local civilization_type = dummy_data.Type
      local policy_type = dummy_data.PolicyType

      -- FIX-0005: was `for _, player in ipairs(Players) do` (skips player 0)
      for _, player in lekmod_dummy_players() do
         if player:IsEverAlive() and player:GetCivilizationType() == GameInfoTypes[civilization_type] then
            if not player:HasPolicy(GameInfoTypes[policy_type]) then
               player:SetNumFreePolicies(1)
               player:SetNumFreePolicies(0)
               player:SetHasPolicy(GameInfoTypes[policy_type], true)
            end
         end
      end

   end

end
-- FIX-0007 (proposal, NO code change): Events.* are local engine events and this handler mutates game state
-- (SetNumFreePolicies / SetHasPolicy). It runs on every load, and on a resync only for the resyncing peer. The HasPolicy
-- guard makes it idempotent for the dummy policy itself, but SetNumFreePolicies(1)/(0) zeroes the free policies the player
-- has accumulated whenever the dummy policy is missing. Proposed fix: move it to a synchronous point (e.g. the first
-- GameEvents.PlayerDoTurn of each player) and keep the guard; this shifts the grant by up to one turn, so it is left to the user.
Events.SequenceGameInitComplete.Add(lekmod_add_dummy_policies)
------------------------------------------------------------------------------------------------------------------------
