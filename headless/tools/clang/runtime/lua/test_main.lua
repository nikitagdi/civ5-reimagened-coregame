-- Scenario `lua` (run.sh --scenario lua): exercises the exe-Lua of rt_lua. Any failed check raises an error, which the
-- host counts and logs. The selftest (rt_lua_selftest.cpp) sets up PreGame (7 minor civs, era 2, max turns 123) and the
-- localization key TXT_KEY_RT_TEST_TURN before running this file.
local checks = 0
local function check(cond, what)
  checks = checks + 1
  if not cond then error('check failed: ' .. what, 2) end
end

print('hello', 1, true, nil, {})

-- include: stem match, case-insensitive, with or without ".lua", executed in the caller's environment
check(include('Rt_Test_Lib') == true, 'include returns true')
check(RT_TEST_LIB_LOADED == 42, 'included file ran in the caller environment')
check(include('rt_test_lib.lua') == true, 'include with extension')
check(include('subdir/rt_test_sub') == true and RT_TEST_SUB_LOADED == 7, 'include finds a file in a subfolder')
check(include('rt_no_such_file') == false, 'include of a missing file returns false')

-- Locale
check(Locale.ConvertTextKey('TXT_KEY_RT_TEST_TURN', 5, 'Terra') == 'Turn 5 on Terra', 'Locale.ConvertTextKey with arguments')
check(Locale.HasTextKey('TXT_KEY_RT_TEST_TURN') == true, 'Locale.HasTextKey')
check(Locale.HasTextKey('TXT_KEY_RT_NO_SUCH_KEY') == false, 'Locale.HasTextKey missing')
check(Locale.ToUpper('abc') == 'ABC' and Locale.ToLower('ABC') == 'abc', 'Locale.ToUpper/ToLower')
Locale.SomethingUnimplemented('x')  -- auto-stub, must not raise

-- PreGame
check(PreGame.GetNumMinorCivs() == 7, 'PreGame.GetNumMinorCivs')
check(PreGame.GetEra() == 2, 'PreGame.GetEra')
check(PreGame.GetMaxTurns() == 123, 'PreGame.GetMaxTurns')
check(type(PreGame.GetWorldSize()) == 'number', 'PreGame.GetWorldSize')
check(type(PreGame.IsHuman(0)) == 'boolean', 'PreGame.IsHuman')
check(type(PreGame.GetSlotStatus(0)) == 'number', 'PreGame.GetSlotStatus')
check(type(PreGame.GetMapScript()) == 'string', 'PreGame.GetMapScript')
check(PreGame.IsMultiplayerGame() == false, 'PreGame.IsMultiplayerGame')
check(SlotStatus.SS_TAKEN == 3 and SlotClaim.SLOTCLAIM_ASSIGNED == 2, 'SlotStatus / SlotClaim enums')
check(not pcall(PreGame.IsHuman, 9999), 'PreGame.IsHuman rejects an out-of-range player')

-- Events / GameEvents / LuaEvents: Add, call, Count, Remove
local sum = 0
local function onTest(a, b) sum = sum + a + (b or 0) end
Events.RtTestEvent.Add(onTest)
check(Events.RtTestEvent.Count() == 1, 'Events Count after Add')
Events.RtTestEvent(2, 3)
check(sum == 5, 'Events call reaches the listener')
Events.RtTestEvent.Remove(onTest)
Events.RtTestEvent(10)
check(sum == 5 and Events.RtTestEvent.Count() == 0, 'Events Remove')
GameEvents.RtTestHook.Add(function(a, b) return a + b == 5 end)       -- called by the host through CallHook
GameEvents.RtTestAccum.Add(function() return 4 end)                    -- called by the host through CallAccumulator
GameEvents.RtTestAccum.Add(function() return 6 end)
LuaEvents.RtTestLua.Add(function() sum = sum + 100 end)
LuaEvents.RtTestLua()
check(sum == 105, 'LuaEvents')
Events.RtHostFired.Add(function() RT_HOST_FIRED = (RT_HOST_FIRED or 0) + 1 end)

-- UI tables are no-op receivers (SIMP-015)
UI.SelectUnit(nil)
Controls.SomeBox:SetHide(true)
Controls.SomeList.Items.Add('x')
Network.SendResearch(1, 2, 3, false)
ContextPtr:SetUpdate(function() end)
check(Mouse.GetPosition() == nil, 'stub call returns nil')

-- sandbox
check(io == nil and dofile == nil and require == nil and loadfile == nil, 'io / dofile / require removed')
check(os.time() == 1356998400, 'os.time is the fixed virtual epoch')
check(os.clock() < os.clock(), 'os.clock is a virtual, increasing clock')
math.randomseed(12345)
local r1 = { math.random(100), math.random(100), math.random() }
math.randomseed(12345)
local r2 = { math.random(100), math.random(100), math.random() }
check(r1[1] == r2[1] and r1[2] == r2[2] and r1[3] == r2[3], 'math.random is deterministic for a seed')
check(r1[1] >= 1 and r1[1] <= 100, 'math.random(n) range')

-- exe library extensions
local filled = table.fill(0, 4)
check(#filled == 4 and filled[1] == 0 and filled[4] == 0, 'table.fill(value, n)')
check(table.count({ a = 1, b = 2, 3 }) == 3, 'table.count')
check(math.clamp(5, 0, 3) == 3 and math.clamp(-1, 0, 3) == 0 and math.clamp(2, 0, 3) == 2, 'math.clamp')
local hex = ToHexFromGrid({ x = 5, y = 4 })
check(hex.x == 3 and hex.y == 4, 'ToHexFromGrid')
local gx, gy = ToGridFromHex(3, 4)
check(gx == 5 and gy == 4, 'ToGridFromHex')
check(type(HexToWorld(hex).x) == 'number', 'HexToWorld')
check(type(GameDefines) == 'table', 'GameDefines table')

-- Modding.OpenSaveData (SimpleValues in ./Civ5SavedGameDatabase.db, in memory)
local save = Modding.OpenSaveData()
save.SetValue('rt_number', 42)
save.SetValue('rt_text', 'forty-two')
check(save.GetValue('rt_number') == 42, 'SaveData number round trip')
check(save.GetValue('rt_text') == 'forty-two', 'SaveData text round trip')
check(save.GetValue('rt_missing') == nil, 'SaveData missing value')
check(Modding.OpenSaveData() == save, 'OpenSaveData returns one object')

print('lua checks passed: ' .. checks)
