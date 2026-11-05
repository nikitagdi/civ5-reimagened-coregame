-- Author: EnormousApplePie

include("Lekmod_utilities.lua")

local this_civ = GameInfoTypes["CIVILIZATION_ITALY"]
local is_active = LekmodUtilities:is_civilization_active(this_civ)

------------------------------------------------------------------------------------------------------------------------
-- Italy UA: Enter a golden age or extend one whenever a player finishes a policy tree
------------------------------------------------------------------------------------------------------------------------
function lekmod_italy_ua_on_policy_adopt(player_id, policy_id)

   print("Italy UA: Policy Adopted")
	local player = Players[player_id]
   local policy = GameInfo.Policies[policy_id]
   local policy_branch = GameInfo.PolicyBranchTypes[policy.PolicyBranchType]

	if not player:IsEverAlive() or player:GetCivilizationType() ~= this_civ
   or policy_branch == nil or not player:HasPolicy(policy_id)
   or not player:IsPolicyBranchFinished(policy_branch.ID)
   -- FIX-0003 / DS-0003: the golden-age bonus below is game state, so it must be applied on every machine and for every
   -- Italy owner (AI included). The original line gated it on the local active player, so only the owner's machine got it
   -- (out of sync in multiplayer, never applied to an AI Italy). GetActivePlayer() now only guards the UI alert.
   -- or Game.GetActivePlayer() ~= player_id then return end
   then return end

   -- amount made for quick speed, but scales with other speeds.
   local golden_age_turns = 5 * (GameInfo.GameSpeeds[Game.GetGameSpeedType()].GoldenAgePercent / 100);
   local golden_age_points = 312.5 * (GameInfo.GameSpeeds[Game.GetGameSpeedType()].GoldenAgePercent / 100)

   if player:IsGoldenAge() then
      player:ChangeGoldenAgeTurns(golden_age_turns)
      -- FIX-0003 / DS-0003: UI alert only for the local player (original: unconditional, the whole handler was local-only)
      if Game.GetActivePlayer() == player_id then
         local popup_text = Locale.ConvertTextKey("TXT_KEY_TRAIT_ITALY_POLICY_BRANCH_FINISHED_EXTENDED", golden_age_turns)
         Events.GameplayAlertMessage(popup_text)
      end
   else
      player:ChangeGoldenAgeProgressMeter(golden_age_points)
      -- FIX-0003 / DS-0003: UI alert only for the local player
      if Game.GetActivePlayer() == player_id then
         local popup_text = Locale.ConvertTextKey("TXT_KEY_TRAIT_ITALY_POLICY_BRANCH_FINISHED", golden_age_points)
         Events.GameplayAlertMessage(popup_text)
      end
   end

end
------------------------------------------------------------------------------------------------------------------------
if is_active then
   GameEvents.PlayerAdoptPolicy.Add(lekmod_italy_ua_on_policy_adopt)
end