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

`GC_EVENT_POPUP_INFO` has the eight-value prefix
`(type, data1, data2, data3, player, flags, option1, option2)`, with localized
optional `text`. Array popups use flags zero and their first two booleans;
absent integers are -1 and absent booleans are false. `BUTTONPOPUP_CITY_CAPTURED`
appends `data4` (captured Great Works count) and `data5` (player eligible for
liberation, or -1). Thus capture events have ten values. This uses the existing
ABI pointer/count; consumers must accept the ten-value shape before deploying.

SIMPLIFIED(SIMP-530): pending command/move/attack/plunder confirmations have no
server-owned unit/command record to read. The original handlers rely on client
selection and interface-mode state. No guessed selection accessor is provided;
the host must retain authenticated action context before showing such a prompt.

The `replicaui` runtime scenario checks popup payloads without a replica sink,
null/short arrays, two explicit seats, invalid arguments, unchanged scratch
storage and game digests. Run it on both native and WASM builds with game data.
