-- GameInfo / DB.Query probe (scenario `gameinfo`, task B1 A5). The same file runs in two hosts:
--   civhost (reference): loaded as a gameplay context (--lua-overlay with this folder, scenario "lua_contexts":
--     ["gameinfo_probe"]); prints every row as well (GI# lines) for diagnosis.
--   rt_lua (portable): rt_selftest_gameinfo runs it with GI_PROBE_QUIET = true and compares the GI| lines with
--     civhost_reference.txt (the GI| lines civhost printed, see README in this folder).
-- Each query is a real usage from the 37 gameplay contexts or the Lekmap map script. A query line is
--   GI|Qnn|<label>|rows=<n>|fields=<n>|h=<hash>
-- where the hash covers every row in iteration order with its fields sorted by name, each value tagged with its
-- Lua type. S lines record semantics (types, identity, missing keys, errors) the scripts rely on.

local quiet = GI_PROBE_QUIET

local function V(v)
	local t = type(v)
	if t == "number" then
		if v == math.floor(v) and v > -9007199254740992 and v < 9007199254740992 then return "n" .. string.format("%.0f", v) end
		return "f" .. string.format("%.14g", v)
	elseif t == "string" then return "s" .. v
	elseif t == "boolean" then return v and "T" or "F"
	elseif t == "nil" then return "nil"
	end
	return t
end

local function RowText(row)
	if type(row) ~= "table" then return "<" .. type(row) .. ">", 0 end
	local keys = {}
	for k in pairs(row) do keys[#keys + 1] = k end
	table.sort(keys, function(a, b) return V(a) < V(b) end)
	local parts = {}
	for i, k in ipairs(keys) do parts[i] = V(k) .. "=" .. V(row[k]) end
	return table.concat(parts, ","), #keys
end

local function Hash(h, s)
	for i = 1, #s do h = (h * 31 + string.byte(s, i)) % 4294967296 end
	return h
end

local function Out(s) print("GI|" .. s) end

local function Q(id, label, fn)
	local rows, fields, h = 0, 0, 7
	local function emit(row)
		local text, n = RowText(row)
		if row ~= nil then rows = rows + 1 end
		fields = fields + n
		h = Hash(h, text .. "\n")
		if not quiet then print(string.format("GI#Q%02d %s", id, text)) end
	end
	local ok, err = pcall(fn, emit)
	if not ok then
		Out(string.format("Q%02d|%s|error", id, label))
		print(string.format("GI#Q%02d error: %s", id, tostring(err)))
		return
	end
	Out(string.format("Q%02d|%s|rows=%d|fields=%d|h=%.0f", id, label, rows, fields, h))
end

-- S lines: a value or "error"; the message goes to a GI# line (not compared: chunk names differ between hosts).
local function S(name, fn)
	local ok, a = pcall(fn)
	if ok then Out("S|" .. name .. "|" .. tostring(a)) else
		Out("S|" .. name .. "|error")
		print("GI#S " .. name .. " error: " .. tostring(a))
	end
end

local function count(iter, s, v)
	local n = 0
	for _ in iter, s, v do n = n + 1 end
	return n
end

-- ---- queries -------------------------------------------------------------------------------------------------------
-- Lekmod_*.lua / UI (unit, civ, policy and building lookups by Type and by ID)
Q(1, "Units[\"UNIT_WARRIOR\"]", function(e) e(GameInfo.Units["UNIT_WARRIOR"]) end)                     -- M1
Q(2, "Units.UNIT_WORKER", function(e) e(GameInfo.Units.UNIT_WORKER) end)                                -- lekmod_argentina
Q(3, "Units.UNIT_AIRSHIP", function(e) e(GameInfo.Units.UNIT_AIRSHIP) end)
Q(4, "Units.UNIT_ARGENTINA_WORKER", function(e) e(GameInfo.Units.UNIT_ARGENTINA_WORKER) end)
Q(5, "Units.UNIT_HELICOPTER_GUNSHIP", function(e) e(GameInfo.Units.UNIT_HELICOPTER_GUNSHIP) end)
Q(6, "Units[UNIT_WARRIOR.ID]", function(e) e(GameInfo.Units[GameInfo.Units.UNIT_WARRIOR.ID]) end)       -- Units[unit:GetUnitType()]
Q(7, "Units[0]", function(e) e(GameInfo.Units[0]) end)
Q(8, "Units[229]", function(e) e(GameInfo.Units[229]) end)                                              -- Units[colorado_unit_id]
Q(9, "Civilizations[\"CIVILIZATION_TONGA\"]", function(e) e(GameInfo.Civilizations["CIVILIZATION_TONGA"]) end)
Q(10, "Civilizations[\"CIVILIZATION_ASSYRIA\"]", function(e) e(GameInfo.Civilizations["CIVILIZATION_ASSYRIA"]) end)
Q(11, "Civilizations[\"CIVILIZATION_IROQUOIS\"]", function(e) e(GameInfo.Civilizations["CIVILIZATION_IROQUOIS"]) end)
Q(12, "Civilizations[\"CIVILIZATION_KILWA\"]", function(e) e(GameInfo.Civilizations["CIVILIZATION_KILWA"]) end)
Q(13, "Civilizations[0]", function(e) e(GameInfo.Civilizations[0]) end)                                  -- [player:GetCivilizationType()]
Q(14, "Policies[\"POLICY_MERCHANT_NAVY\"]", function(e) e(GameInfo.Policies["POLICY_MERCHANT_NAVY"]) end)
Q(15, "Policies[\"POLICY_TREASURE_FLEETS\"]", function(e) e(GameInfo.Policies["POLICY_TREASURE_FLEETS"]) end)
Q(16, "Policies[10]", function(e) e(GameInfo.Policies[10]) end)                                          -- Policies[policyID]
Q(17, "PolicyBranchTypes[policy.PolicyBranchType]", function(e)
	e(GameInfo.PolicyBranchTypes[GameInfo.Policies.POLICY_TREASURE_FLEETS.PolicyBranchType]) end)
Q(18, "Buildings[0]", function(e) e(GameInfo.Buildings[0]) end)                                          -- Buildings[buildingID]
Q(19, "BuildingClasses[building.BuildingClass]", function(e)
	e(GameInfo.BuildingClasses[GameInfo.Buildings[0].BuildingClass]) end)                                 -- lekmod_uae
Q(20, "InterfaceModes[0]", function(e) e(GameInfo.InterfaceModes[0]) end)
Q(21, "Buildings()", function(e) for row in GameInfo.Buildings() do e(row) end end)                       -- prophetreplace
Q(22, "Global_Dummy_Buildings()", function(e) for row in GameInfo.Global_Dummy_Buildings() do e(row) end end)
Q(23, "Civilization_Dummy_Policies()", function(e) for row in GameInfo.Civilization_Dummy_Policies() do e(row) end end)
Q(24, "Improvement_Adjacency_Yields()", function(e) for row in GameInfo.Improvement_Adjacency_Yields() do e(row) end end)
Q(25, "Policy_FreePromotionUnitCombats()", function(e) for row in GameInfo.Policy_FreePromotionUnitCombats() do e(row) end end)
-- Lekmap map script (LekmapPangaeaFractal + HB* libraries)
Q(26, "Worlds.WORLDSIZE_DUEL", function(e) e(GameInfo.Worlds.WORLDSIZE_DUEL) end)
Q(27, "Worlds.WORLDSIZE_HUGE", function(e) e(GameInfo.Worlds.WORLDSIZE_HUGE) end)
Q(28, "Worlds[3]", function(e) e(GameInfo.Worlds[3]) end)                                                -- Worlds[Map.GetWorldSize()]
Q(29, "Worlds()", function(e) for row in GameInfo.Worlds() do e(row) end end)
Q(30, "Terrains.TERRAIN_GRASS", function(e) e(GameInfo.Terrains.TERRAIN_GRASS) end)
Q(31, "Eras[0]", function(e) e(GameInfo.Eras[0]) end)                                                    -- Eras[startEra].NoGoodies
Q(32, "Climates[1]", function(e) e(GameInfo.Climates[1]) end)                                            -- Climates[Map.GetClimate()]
Q(33, "GameSpeeds[GAMESPEED_STANDARD.ID]", function(e) e(GameInfo.GameSpeeds[GameInfo.GameSpeeds.GAMESPEED_STANDARD.ID]) end)
Q(34, "Regions[\"REGION_JUNGLE\"]", function(e) e(GameInfo.Regions["REGION_JUNGLE"]) end)                 -- Regions[v].ID
Q(35, "Features()", function(e) for row in GameInfo.Features() do e(row) end end)
Q(36, "Improvements()", function(e) for row in GameInfo.Improvements() do e(row) end end)
Q(37, "Resources()", function(e) for row in GameInfo.Resources() do e(row) end end)
Q(38, "Natural_Wonder_Placement()", function(e) for row in GameInfo.Natural_Wonder_Placement() do e(row) end end)
Q(39, "Natural_Wonder_Placement[1]", function(e) e(GameInfo.Natural_Wonder_Placement[1]) end)            -- [rn]
Q(40, "Civilization_Start_Along_Ocean{CARTHAGE}", function(e)
	for row in GameInfo.Civilization_Start_Along_Ocean{CivilizationType = "CIVILIZATION_CARTHAGE"} do e(row) end end)
Q(41, "Civilization_Start_Along_River{AKKAD}", function(e)
	for row in GameInfo.Civilization_Start_Along_River{CivilizationType = "CIVILIZATION_AKKAD"} do e(row) end end)
Q(42, "Civilization_Start_Place_First_Along_Ocean{CARTHAGE}", function(e)
	for row in GameInfo.Civilization_Start_Place_First_Along_Ocean{CivilizationType = "CIVILIZATION_CARTHAGE"} do e(row) end end)
Q(43, "Civilization_Start_Region_Priority {VIETNAM}", function(e)
	for row in GameInfo.Civilization_Start_Region_Priority {CivilizationType = "CIVILIZATION_VIETNAM"} do e(row) end end)
Q(44, "Civilization_Start_Region_Avoid{EGYPT}", function(e)
	for row in GameInfo.Civilization_Start_Region_Avoid{CivilizationType = "CIVILIZATION_EGYPT"} do e(row) end end)
Q(45, "DB.Query count Region_Priority ?", function(e)
	for row in DB.Query("select count(*) as count from Civilization_Start_Region_Priority where CivilizationType = ?", "CIVILIZATION_VIETNAM") do e(row) end end)
Q(46, "DB.Query count Region_Avoid ?", function(e)
	for row in DB.Query("select count(*) as count from Civilization_Start_Region_Avoid where CivilizationType = ?", "CIVILIZATION_EGYPT") do e(row) end end)
-- spec forms: filter iteration, all rows, missing table / row, DB.Query with SQL
Q(47, "Units{Combat=8}", function(e) for row in GameInfo.Units{Combat = 8} do e(row) end end)
Q(48, "Units()", function(e) for row in GameInfo.Units() do e(row) end end)
Q(49, "NoSuchTable", function(e) e(GameInfo.NoSuchTable) end)
Q(50, "DB.Query Units Combat > ?", function(e)
	for row in DB.Query("SELECT ID, Type, Combat, Domain FROM Units WHERE Combat > ? ORDER BY ID", 30) do e(row) end end)

-- ---- semantics -----------------------------------------------------------------------------------------------------
S("type_GameInfo", function() return type(GameInfo) end)
S("type_table", function() return type(GameInfo.Units) end)
S("table_cached", function() return GameInfo.Units == GameInfo.Units end)
S("table_by_bracket", function() return GameInfo["Units"] == GameInfo.Units end)
S("table_lowercase", function() return GameInfo.units ~= nil end)
S("type_row", function() return type(GameInfo.Units[0]) end)
S("row_cached", function() return GameInfo.Units[0] == GameInfo.Units[0] end)
S("row_type_eq_id", function() return GameInfo.Units.UNIT_WARRIOR == GameInfo.Units[GameInfo.Units.UNIT_WARRIOR.ID] end)
S("row_iter_eq_index", function() local f = GameInfo.Units() local r = f() return r == GameInfo.Units[r.ID] end)
S("row_metatable", function() return getmetatable(GameInfo.Units[0]) ~= nil end)
S("row_missing_type", function() return GameInfo.Units["UNIT_NO_SUCH"] end)
S("row_missing_id", function() return GameInfo.Units[100000] end)
S("row_negative_id", function() return GameInfo.Units[-1] end)
S("row_fraction_id", function() local r = GameInfo.Units[1.5] return r and r.Type end)
S("row_numeric_string", function() local r = GameInfo.Units["1"] return r and r.Type end)
S("row_bool_key", function() return GameInfo.Units[true] end)
S("row_by_id_field_string", function() return GameInfo.Units["ID"] end)
S("call_returns", function() return select("#", GameInfo.Units()) end)
S("call_type", function() return type((GameInfo.Units())) end)
S("call_empty_filter", function() return count(GameInfo.Units{}) end)
S("call_where_string", function() return count(GameInfo.Units("Combat > 30")) end)
S("call_where_string_and", function() return count(GameInfo.Units("Combat > 30 and Domain = 'DOMAIN_SEA'")) end)
S("call_multi_filter", function() return count(GameInfo.Units{Combat = 8, Domain = "DOMAIN_LAND"}) end)
S("call_filter_numeric_string", function() return count(GameInfo.Units{Combat = "8"}) end)
S("call_filter_bool", function() return count(GameInfo.Eras{NoGoodies = true}) end)
S("call_filter_bool_false", function() return count(GameInfo.Eras{NoGoodies = false}) end)
S("call_filter_nomatch", function() return count(GameInfo.Units{Type = "UNIT_NO_SUCH"}) end)
-- Units{NoSuchColumn = 1}: civhost (the original CvGameDatabase) crashes in Results::Bind on the failed statement; not probed.
S("call_where_bad_sql", function() return count(GameInfo.Units("NoSuchColumn = 1")) end)
S("call_where_empty_string", function() return count(GameInfo.Units("")) end)
S("call_number_arg", function() return count(GameInfo.Units(5)) end)
S("call_filter_fraction", function() return count(GameInfo.Units{ID = 1.5}) end)
S("call_filter_bool_id", function() return count(GameInfo.Eras{ID = true}) end)
S("call_filter_two_bools", function() return count(GameInfo.Eras{NoGoodies = true, NoBarbUnits = false}) end)
S("row_filter_eq_index", function() for r in GameInfo.Units{Combat = 8} do return r == GameInfo.Units[r.ID] end end)
S("row_where_eq_index", function() for r in GameInfo.Units("Combat = 8") do return r == GameInfo.Units[r.ID] end end)
S("row_nil_key", function() return GameInfo.Units[nil] end)
S("row_mt_keys", function() local mt = getmetatable(GameInfo.Units[0]) if type(mt) ~= "table" then return type(mt) end
	local ks = {} for k in pairs(mt) do ks[#ks + 1] = tostring(k) end table.sort(ks) return table.concat(ks, " ") end)
S("table_mt_keys", function() local mt = getmetatable(GameInfo.Units) if type(mt) ~= "table" then return type(mt) end
	local ks = {} for k in pairs(mt) do ks[#ks + 1] = tostring(k) end table.sort(ks) return table.concat(ks, " ") end)
S("gameinfo_mt_keys", function() local mt = getmetatable(GameInfo) if type(mt) ~= "table" then return type(mt) end
	local ks = {} for k in pairs(mt) do ks[#ks + 1] = tostring(k) end table.sort(ks) return table.concat(ks, " ") end)
S("gameinfo_raw_units", function() return rawget(GameInfo, "Units") ~= nil end)
S("gameinfo_pairs_ge_400", function() local n = 0 for k in pairs(GameInfo) do n = n + 1 end return n >= 400 end)
S("row_rawset", function() local r = GameInfo.Worlds[1] rawset(r, "ProbeRaw", 2) return GameInfo.Worlds[1].ProbeRaw end)
S("row_iter_missing_field", function() return GameInfo.Worlds[0].NoSuchField end)
S("defines_mt_index", function() local r = GameInfo.Defines["MAX_MAJOR_CIVS"] return r ~= nil end)
S("call_error_deferred", function() local ok = pcall(function() local f = GameInfo.Units{} end) return ok end)
S("row_index_half", function() return GameInfo.Units[2.5].ID end)
S("row_index_neg_fraction", function() local r = GameInfo.Worlds[-0.4] return r and r.ID end)
S("call_filter_round", function() return count(GameInfo.Worlds{ID = 5.5}) end)
S("call_filter_half", function() for r in GameInfo.Units{ID = 2.5} do return r.ID end end)
S("table_newindex", function() GameInfo.Units.Foo = 1 return "assigned" end)
S("gameinfo_assign", function() GameInfo.ProbeAssigned = 1 return rawget(GameInfo, "ProbeAssigned") end)
S("gameinfo_number_key", function() return GameInfo[1] end)
S("row_lower_field", function() return GameInfo.Worlds[0].type end)
S("dbrow_metatable", function() for r in DB.Query("select 1 as x") do return getmetatable(r) ~= nil end end)
S("dbrow_write", function() for r in DB.Query("select 1 as x") do r.x = 5 return r.x end end)
S("db_query_text_bool", function() for r in DB.Query("select AnyIdeologyUnlock from Units where ID = 83") do return V(r.AnyIdeologyUnlock) end end)
S("row_text_in_bool_column", function() return V(GameInfo.Units.UNIT_WARRIOR.AnyIdeologyUnlock) end)
S("type_key_with_quote", function() return GameInfo.Units["UNIT_'X"] end)
S("len_no_id_table", function() return #GameInfo.Unit_FreePromotions end)
S("no_id_rowid_eq_first", function() return GameInfo.Unit_FreePromotions[1] == GameInfo.Unit_FreePromotions()() end)
S("call_extra_arg", function() return count(GameInfo.Units({Combat = 8}, "x")) end)
S("where_type_range_order", function() local t = {} for r in GameInfo.Units("Type > 'UNIT_W'") do t[#t + 1] = r.ID if #t == 4 then break end end return table.concat(t, ",") end)
S("filter_type_order", function() local t = {} for r in GameInfo.Units{Domain = "DOMAIN_SEA"} do t[#t + 1] = r.ID if #t == 4 then break end end return table.concat(t, ",") end)
S("db_query_two_params", function() return count(DB.Query("select * from Units where Combat = ? and Domain = ?", 8, "DOMAIN_LAND")) end)
S("db_query_param_fraction", function() return count(DB.Query("select * from Units where ID = ?", 1.5)) end)S("call_iter_after_end", function() local f = GameInfo.Worlds{Type = "WORLDSIZE_DUEL"} local a = f() local b = f() local c = f() return tostring(a ~= nil) .. tostring(b) .. tostring(c) end)
S("len_table", function() return #GameInfo.Units end)
S("pairs_table", function() local n = 0 for k, v in pairs(GameInfo.Worlds) do n = n + 1 end return n end)
S("row_write_persists", function() local r = GameInfo.Worlds[0] r.ProbeField = 1 return GameInfo.Worlds[0].ProbeField end)
S("defines_by_name", function() local r = GameInfo.Defines.MAX_MAJOR_CIVS return r and V(r.Value) end)
S("defines_iter", function() return count(GameInfo.Defines()) end)
S("defines_first", function() local f = GameInfo.Defines() return RowText(f()) end)
S("defines_real", function() for r in GameInfo.Defines{Name = "CITY_GROWTH_EXPONENT"} do return V(r.Value) end end)
S("defines_text", function() for r in GameInfo.Defines{Name = "QUICKSAVE"} do return V(r.Value) end end)
S("no_id_table_index", function() local r = GameInfo.Unit_FreePromotions[1] return r and RowText(r) end)
S("no_id_table_type_index", function() return GameInfo.Unit_FreePromotions["UNIT_WARRIOR"] end)
S("missing_table_call", function() return GameInfo.NoSuchTable() end)
S("localization_table", function() return GameInfo.Language_en_US ~= nil end)
S("db_query_type", function() return type(DB.Query("select 1 as x")) end)
S("db_query_noresult", function() return count(DB.Query("select * from Units where ID = -5")) end)
S("db_query_bad_sql", function() return count(DB.Query("select from")) end)
S("db_query_null", function() for r in DB.Query("select NULL as a, 1 as b") do return RowText(r) end end)
S("db_query_bool_column", function() for r in DB.Query("select NoGoodies from Eras where ID = 0") do return V(r.NoGoodies) end end)
S("db_query_bool_param", function() return count(DB.Query("select * from Eras where NoGoodies = ?", true)) end)
S("db_query_float", function() for r in DB.Query("select 1.5 as x, 2.0 as y") do return V(r.x) .. V(r.y) end end)
S("db_query_dup_names", function() for r in DB.Query("select 1 as a, 2 as a") do return V(r.a) end end)
S("bool_null_column", function() for r in DB.Query("select typeof(NoGoodies) as t from Eras where NoGoodies is null") do return r.t end return "none" end)
Out("done")
