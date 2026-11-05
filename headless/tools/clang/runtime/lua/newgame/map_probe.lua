-- map_probe.lua: prints the canonical map text of RtSession::MapText (headless/portable/runtime/rt_session.h) as MP| lines,
-- so the same map digest (FNV-1a 64 of that text) can be taken in rt_session (scenario newgame) and in civhost.
-- Lines: "MP|H|<w> <h>", per row y chunks of up to 16 plots "MP|R|<y>|<x0>|<cells>", then "MP|done".
-- Cell: plotType,terrain,feature,resource(NO_TEAM),numResource,NEOfRiver,WOfRiver,NWOfRiver,riverE,riverSE,riverSW;
-- civhost (Windows; OUT = an empty folder, OVERLAY = a folder holding only this file, scenario lua_contexts = ["map_probe"]):
--   civhost.exe --stage map --scenario <scenario.json> --lua-overlay OVERLAY --out OUT
-- then the "map_probe: MP|" lines of OUT\lua.log.
local w, h = Map.GetGridSize()
print("MP|H|" .. w .. " " .. h)
local function b(v) if v then return 1 end return 0 end
for y = 0, h - 1 do
	local parts = {}
	local x0 = 0
	for x = 0, w - 1 do
		local p = Map.GetPlot(x, y)
		parts[#parts + 1] = string.format("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d;", p:GetPlotType(), p:GetTerrainType(), p:GetFeatureType(),
			p:GetResourceType(-1), p:GetNumResource(), b(p:IsNEOfRiver()), b(p:IsWOfRiver()), b(p:IsNWOfRiver()),
			p:GetRiverEFlowDirection(), p:GetRiverSEFlowDirection(), p:GetRiverSWFlowDirection())
		if #parts == 16 or x == w - 1 then
			print("MP|R|" .. y .. "|" .. x0 .. "|" .. table.concat(parts))
			parts = {}
			x0 = x + 1
		end
	end
end
print("MP|done")
