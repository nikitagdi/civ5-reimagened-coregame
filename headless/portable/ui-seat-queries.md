# Seat queries and popup payloads

These headless additions are registered as Lua globals by `gc_lua_attach_ui`.
The host must authorize the requested seat before calling them. An explicit player
argument selects the data; it does not authenticate the caller.

* `GcUiGetAdvisorCounsel(player)` returns the same shape as
  `Game.GetAdvisorCounsel()`: advisor enum keys, each containing zero-based text
  entries. It builds a temporary counsel list, leaving the global list alone.
* `GcUiGetCurrentDeals(player)` and `GcUiGetHistoricDeals(player)` return
  one-based lists of plain deal tables, in the core's order.
* `GcUiGetProposedDeal(player, from, to)` returns a plain deal table or `nil`.
  The explicit player must be one of the two parties.
* `GcUiGetBullyScores(player, minor)` returns two integers: the tribute
  pressure of the player on a living city-state for gold and for a worker
  (`CvMinorCivAI::CalculateBullyMetric`). These are the scores that
  `GetMajorBullyGoldDetails` and `GetMajorBullyUnitDetails` print. Tribute is
  possible at zero and above. It makes the same calls as those getters, so it
  adds no state change of its own.

* `GcUiLeaderGreeting(seat, ai, count)` returns the greeting of
  `CvDiplomacyAI::DoBeginDiploWithHuman` for the seat and its animation (nil under
  Always War). `count` is the seat's own start-diplomacy count, which
  `RtUserInterface::GetStartDiploRepeatCount` answers during the call only.
* `GcUiDiploText(seat, ai, name)` returns the leader's line
  `GetDiploStringForMessage` builds for the seat. Names: `LETS_HEAR_IT`,
  `TRADE_REJECT_UNACCEPTABLE`, `TRADE_REJECT_INSULTING`, `REPEAT_TRADE`,
  `REPEAT_TRADE_TOO_MUCH`, `TRADE_DEAL_UNCHANGED`, `TRADE_NO_DEAL_POSSIBLE`,
  `TRADE_CANT_MATCH_OFFER`, `TRADE_AI_MAKES_OFFER`, `PEACE_WHAT_WILL_HUMAN_OFFER`,
  `NO_PEACE`, `TOO_SOON_NO_PEACE`.
* `GcUiDealQuery(seat, partner, items, op[, mode])` works on a local deal from the
  seat to a met major, built from `items` (a list of `{type, from, data1, data2,
  data3, flag1}`, `from` one of the two parties, at most 64) with the deal's own
  `Add*` calls. `op` is `"check"` (one boolean per item: the core took it after
  the ones before), `"pockets"` (`{from, type, data1, data2, max}` rows of what
  each side may still add: gold, gold per turn, the tradeable resources, embassy,
  open borders, defensive pact, research agreement, peace), `"evaluate"` (an AI
  partner's verdict as `DoHumanOfferDealToThisAI` makes it: `response` is
  `accept`, `unacceptable` or `insulting`, with `value`, `mine`, `theirs`, `peace`
  and `cant_match`; `value` is what `sendNetDealAccepted` carries), `"equalize"`
  (`DoEqualizeDealWithHuman` on the local deal, `mode` 0 balance, 1 the AI's items
  stay, 2 the seat's items stay: `ok`, `good`, `cant_match`, `items`) or `"blob"`
  (the hex of `ICvDeal1::Write`; an error when an item was refused).

These diplomacy functions run as the seat (`ActiveAs` switches the pregame field for
the call and nothing else) and raise no Lua error while it is switched. They use no
scratch deal and change no game state; the server runs them inside the read scope.

* `GcUiReadScope(f, ...)` calls `f(...)` and returns its results. Before it
  returns, and also when `f` raises (the error is raised again afterwards), it
  puts back what a Lua getter may change on the asking machine only: the game
  and map RNG (DS-0038, `City:GetNextBuyablePlot` and `City:CanBuyPlot()` draw
  from the game RNG) and every player's military/economic might cache (DS-0034
  through the bully metric: `CanMajorBullyGold`, `GetMajorBullyGoldDetails`,
  `GetMajorBullyUnitDetails`, which FIX-0022 does not route through
  `HeadlessUiMight`), and every player's exploration plots
  (`Unit:CanDoCommand(COMMAND_AUTOMATE, AUTOMATE_EXPLORE)` reaches
  `CvEconomicAI::GetExplorationPlots` through `CvUnit::CanAutomate` and
  `CvHomelandAI::IsAnyValidExploreMoves`, which runs `UpdatePlots` while
  `m_bExplorationPlotsDirty` is set). The economic AI of a player whose plots
  are dirty is saved with its own `Write` and, when the read rebuilt them, put
  back with its `Read`, so the flag stays set and the simulation rebuilds them
  where it would have. Values are restored only where they changed. A host runs
  every screen query and screen action script through it; the action's net
  messages are sent afterwards and run as ordinary commands.

Players must be initialized major civilizations; invalid arguments raise Lua
errors. These queries neither select an active player nor load a scratch deal.
Dead, previously initialized players can still read their history.

A deal table has integer fields `from`, `to`, `start_turn`, `final_turn`,
`duration`, and a one-based `items` list. Each item has integer fields `type`,
`from`, `data1`, `data2`, `data3`, `duration`, `turns_left`, and boolean `flag1`.
`turns_left` is the core's remaining duration, clamped to zero (or the final turn
minus the current turn on builds without remaining-duration storage).
The tables contain values, not references to native deal objects.

```lua
local counsel = GcUiGetAdvisorCounsel(seat)
local current = GcUiGetCurrentDeals(seat)
local history = GcUiGetHistoricDeals(seat)
local proposal = GcUiGetProposedDeal(seat, seat, partner)
for _, deal in ipairs(current) do
  for _, item in ipairs(deal.items) do
    -- item.from identifies the party giving this item.
  end
end
```

`GC_EVENT_DIPLO_LEADER_MESSAGE` (17) reports every leader message the core shows a
human (`gDLL->GameplayDiplomacyAILeaderMessage`, queued by `rt_diplo`): the replies to
the human's own diplomacy, which a delivery builds for its sender, and the AI's
requests. Args: AI, `DiploUIStateTypes`, human, animation, data, request (1 for an
AI request), answer (how `rt_diplo` answered it: 0 declined offer, 1 refused demand,
2 refused request, 3 replied no, 4 dismissed); text: the line. A server hands it to
the seat's leader screen. `GC_EVENT_DIPLO_AUTO_DECLINED` still reports the declined
requests (SIMP-003).

`GC_EVENT_POPUP_INFO` has the eight-value prefix
`(type, data1, data2, data3, player, flags, option1, option2)`, with localized
optional `text`. Array popups use flags zero and their first two booleans;
absent integers are -1 and absent booleans are false. `BUTTONPOPUP_CITY_CAPTURED`
appends `data4` (captured Great Works count) and `data5` (player eligible for
liberation, or -1). Thus capture events have ten values. This uses the existing
ABI pointer/count; consumers must accept the ten-value shape before deploying.

`BUTTONPOPUP_DECLAREWARMOVE` also has ten values: `data4` is the acting unit
and `data5` its `MOVE_TO` mission flags. Like the original executable's move
click, `gc_apply_command` asks `CvUnit::GetDeclareWarMove` before a seated
human's `GC_CMD_UNIT_MOVE_TO` to a visible plot. When it names a rival team,
the command opens this question for that unit (flags 0) and returns `GC_OK`
without sending or journaling anything. Autoplay and journal replays skip this
check. The question is asked only for a player whose host called
`GcUiSetMoveQuestions(player, true)`, meaning that its client answers this popup.
The flag is runtime-only: it is never saved and `gc_create` clears it. For any other
player, the move goes through the ordinary move checks, as before. The question
is also skipped while that player's own war declaration on the rival team
(`GC_CMD_DECLARE_WAR`) waits in the loopback for the next pump, so a declaration
followed at once by the move still works. `CvUnit::UnitAttack` can also raise the question while a mission runs.
Then the acting unit is the popup player's one unit whose head `MOVE_TO`
mission to (`data2`, `data3`) `CvUnitMission::ContinueMission` is evaluating
(`UNITFLAG_EVALUATING_MISSION`). When no single unit matches, `data4` is -1 and
`data5` is 0. Both paths only read game state. They do not touch the mission
queue or the random numbers, so the simulation and its digests do not change.
A host replays a "yes" as the war declaration followed by the same move for
that unit.

SIMPLIFIED(SIMP-530): command and improvement-rebuild confirmations come from
`CvGame::handleAction`, which acts on the client's head-selected unit; a server
has no selection, so those events carry no unit. Range-strike, plunder and gift
confirmations are raised by the original client UI, not by the core.

The `replicaui` runtime scenario checks popup payloads without a replica sink,
null/short arrays, two explicit seats, invalid arguments, unchanged scratch
storage and game digests. Run it on both native and WASM builds with game data.
