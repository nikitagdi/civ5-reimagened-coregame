// Scenarios of the WorldBuilder map loader (gc-abi-v2 4.15, P14; tasks R0.18, R2.11, N3.20):
//  - `wbmapread` (run.sh --scenario wbmapread --maps DIR): every given .Civ5Map through CvWorldBuilderMap::Load with the game's type tables,
//    no game started. A reader of this file (RawMap, written from the format notes of CvWorldBuilderMapLoad.cpp, not from its code) walks
//    the same bytes and every plot, player, team, city and unit stack of the loaded map is compared with it; the layout must use the file
//    to its last byte.
//  - `wbmap` (run.sh --scenario wbmap --map FILE): a network game on the map (keys world_builder_map / world_builder_scenario), the map's
//    plots compared with the file after the start, then --turns turns with the test bot in the human slots; per-turn digests
//    ("wbmap-turn|turn|digest") and the final "state_digest" let run.sh compare two processes.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <memory>
#include <string>
#include <vector>

#define CV_WORLD_BUILDER_MAP_IN_GAME
#include "CvWorldBuilderMap.h"
#include "CvWorldBuilderMapTypeDesc.h"
#include "CvWorldBuilderMapLoader.h"
#include "rt_autoplay.h"
#include "rt_selftest.h"
#include "rt_session.h"
#include "lekmod_portable/host_log.h"
#include "lekmod_portable/wb_map.h"

using namespace lekmod_portable;

namespace {

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

std::string Escape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		const char ch = s[i];
		if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
		else if ((unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7f) out += ch;
	}
	return out;
}

struct Check
{
	std::string json;
	const char* tag;
	int failed;
	explicit Check(const char* t) : tag(t), failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[2048];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + Escape(name) + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("%s: %-4s %s: %s\n", tag, ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

bool ReadHostFile(const gc_host* host, const std::string& path, std::string& out)
{
	gc_buf buf = {0, 0, 0, 0};
	if (!host || !host->read_file || host->read_file(host->user, path.c_str(), &buf) != GC_OK) return false;
	out.assign((const char*)buf.data, buf.len);
	gc_buf_release(&buf);
	return true;
}

std::wstring Wide(const std::string& s)
{
	std::wstring w;
	for (size_t i = 0; i < s.size(); ++i) w += (wchar_t)(unsigned char)s[i];
	return w;
}

// ---- an independent reader of the file ------------------------------------------------------------------
struct RawMap
{
	std::string bytes;
	int version;
	bool scenario;
	uint w, h, flags, playable;
	std::string worldType;
	std::vector<std::string> terrain, feature, wonder, resource, impr, unitTypes, techs, policies, buildings, promos;
	size_t plots, scenPlots, units, cities, players, cityStates, teams, end;
	uint unitsSize, citiesSize, nPlayers, nCityStates, nTeams;
	std::string error;

	RawMap() : version(0), scenario(false), w(0), h(0), flags(0), playable(0), plots(0), scenPlots(0), units(0), cities(0), players(0), cityStates(0), teams(0), end(0),
		unitsSize(0), citiesSize(0), nPlayers(0), nCityStates(0), nTeams(0) {}

	const unsigned char* At(size_t off) const { return (const unsigned char*)bytes.data() + off; }
	uint U32(size_t off) const { uint v = 0; if (off + 4 <= bytes.size()) memcpy(&v, At(off), 4); return v; }
	uint U16(size_t off) const { unsigned short v = 0; if (off + 2 <= bytes.size()) memcpy(&v, At(off), 2); return v; }
	uint U8(size_t off) const { return off < bytes.size() ? *At(off) : 0; }
	uint UnitRecord() const { return version >= 12 ? 0x54 : version >= 8 ? 0x30 : 0xc; }
	uint CityRecord() const { return version >= 12 ? 0x88 : version >= 8 ? 0x68 : 0x64; }
	uint PlayerRecord() const { return version >= 9 ? 0x1b4 : 0x130; }
	uint PlotCount() const { return w * h; }

	static std::vector<std::string> Names(const char* p, size_t n)
	{
		std::vector<std::string> out;
		size_t i = 0;
		while (i < n)
		{
			size_t j = i;
			while (j < n && p[j]) ++j;
			out.push_back(std::string(p + i, j - i));
			i = j + 1;
		}
		return out;
	}
	bool Need(size_t off, size_t n)
	{
		if (off + n > bytes.size()) { error = Format("file ends at %u, needs %u", (unsigned)bytes.size(), (unsigned)(off + n)); return false; }
		return true;
	}

	bool Parse()
	{
		if (!Need(0, 10)) return false;
		version = U8(0) & 0x7f;
		scenario = (U8(0) & 0x80) != 0;
		w = U32(1);
		h = U32(5);
		playable = U8(9);
		size_t off = 10;
		const uint nSizes = version >= 7 ? 8 : 7;
		if (!Need(off, 4 * nSizes)) return false;
		uint s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
		for (uint i = 0; i < nSizes; ++i) s[i] = U32(off + 4 * i);
		off += 4 * nSizes;
		flags = s[0];
		size_t total = 0;
		for (int i = 1; i < 8; ++i) total += s[i];
		if (!Need(off, total)) return false;
		const char* p = (const char*)At(off);
		terrain = Names(p, s[1]);
		feature = Names(p + s[1], s[2]);
		wonder = Names(p + s[1] + s[2], s[3]);
		resource = Names(p + s[1] + s[2] + s[3], s[4]);
		off += total;
		if (version >= 11)
		{
			const uint n = U32(off);
			if (!Need(off, 4 + n)) return false;
			worldType = std::string((const char*)At(off + 4), n).c_str();
			off += 4 + n;
		}
		plots = off;
		off += (size_t)PlotCount() * 8;
		if (!Need(plots, (size_t)PlotCount() * 8)) return false;
		if (scenario)
		{
			const size_t header = version >= 8 ? 0x80 : 0x78;
			if (!Need(off, header)) return false;
			nPlayers = U8(off + 0x50);
			nCityStates = U8(off + 0x51);
			nTeams = U8(off + 0x52);
			// impr, unit types, techs, policies, buildings, [promotions], units, [unit names], cities, victories, options
			std::vector<uint> b;
			for (size_t i = 0x54; i < header; i += 4) b.push_back(U32(off + i));
			off += header;
			size_t q = off;
			const char* t = (const char*)At(q);
			size_t k = 0;
			impr = Names(t, b[k]); t += b[k]; q += b[k++];
			unitTypes = Names(t, b[k]); t += b[k]; q += b[k++];
			techs = Names(t, b[k]); t += b[k]; q += b[k++];
			policies = Names(t, b[k]); t += b[k]; q += b[k++];
			buildings = Names(t, b[k]); t += b[k]; q += b[k++];
			if (version >= 8) { promos = Names(t, b[k]); t += b[k]; q += b[k++]; }
			units = q; unitsSize = b[k]; q += b[k++];
			if (version >= 8) q += b[k++];  // unit names
			cities = q; citiesSize = b[k]; q += b[k++];
			q += b[k++];  // victory types
			q += b[k++];  // game options
			const uint relBits = nTeams ? (nTeams - 1) * nTeams / 2 : 0;
			q += 5 * (relBits / 8 + (relBits % 8 ? 1 : 0));
			if (version >= 10) q += nPlayers * 256;
			const size_t visBits = (size_t)nTeams * PlotCount();
			q += visBits / 8 + (visBits % 8 ? 1 : 0);
			teams = q;
			q += nTeams * 0x40;
			players = q;
			q += nPlayers * PlayerRecord();
			cityStates = q;
			q += nCityStates * PlayerRecord();
			scenPlots = q;
			q += (size_t)PlotCount() * 8;
			off = q;
		}
		end = off;
		if (end != bytes.size()) { error = Format("layout ends at %u, the file at %u", (unsigned)end, (unsigned)bytes.size()); return false; }
		return true;
	}

	std::string Name(const std::vector<std::string>& table, uint index) const { return index < table.size() ? table[index] : std::string(); }
	std::string PlayerCiv(uint i, bool cityState) const
	{
		const size_t base = (cityState ? cityStates : players) + (size_t)i * PlayerRecord() + (version >= 9 ? 0xa0 : 0x20);
		return std::string((const char*)At(base), 64).c_str();
	}
	// Entries of an element allocator block that are in use: the walk of the free list (index 0 never counts as free, as in ConvertTo).
	std::vector<bool> UsedEntries(size_t off, uint size, uint record) const
	{
		const uint n = size >= 4 ? (size - 4) / record : 0;
		std::vector<bool> used(n, true);
		if (version >= 12)
		{
			// Deserialize keeps the list as is: follow it from its head.
			int f = (int)U32(off);
			for (uint steps = 0; f >= 0 && f < (int)n && steps < n; ++steps) { used[f] = false; f = (int)U32(off + 4 + (size_t)f * record); }
		}
		else
		{
			int f = (int)U32(off);
			for (uint steps = 0; f > 0 && f < (int)n && steps < n; ++steps) { used[f] = false; f = (int)U32(off + 4 + (size_t)f * record); }
		}
		return used;
	}
};

// The game's type tables, as CvWorldBuilderMapLoader's InitTypeDesc builds them.
void GameTypeDesc(CvWorldBuilderMapTypeDesc& d)
{
	for (int i = 0; i < GC.getNumTerrainInfos(); ++i) { const CvTerrainInfo* x = GC.getTerrainInfo((TerrainTypes)i); d.m_kTerrainTypes.Add(x ? x->GetType() : NULL); }
	for (int i = 0; i < GC.getNumFeatureInfos(); ++i)
	{
		const CvFeatureInfo* x = GC.getFeatureInfo((FeatureTypes)i);
		d.m_kFeatures.Add(x ? x->GetType() : NULL);
		d.m_kNaturalWonders.Add(x ? x->GetType() : NULL);
	}
	for (int i = 0; i < GC.getNumResourceInfos(); ++i) { const CvResourceInfo* x = GC.getResourceInfo((ResourceTypes)i); d.m_kResources.Add(x ? x->GetType() : NULL); }
	for (int i = 0; i < GC.getNumImprovementInfos(); ++i) { const CvImprovementEntry* x = GC.getImprovementInfo((ImprovementTypes)i); d.m_kImprovements.Add(x ? x->GetType() : NULL); }
	for (int i = 0; i < GC.getNumUnitInfos(); ++i) { const CvUnitEntry* x = GC.getUnitInfo((UnitTypes)i); d.m_kUnits.Add(x ? x->GetType() : NULL); }
	for (int i = 0; i < GC.getNumTechInfos(); ++i) { const CvTechEntry* x = GC.getTechInfo((TechTypes)i); d.m_kTechs.Add(x ? x->GetType() : NULL); }
	for (int i = 0; i < GC.getNumPolicyInfos(); ++i) { const CvPolicyEntry* x = GC.getPolicyInfo((PolicyTypes)i); d.m_kPolicies.Add(x ? x->GetType() : NULL); }
	for (int i = 0; i < GC.getNumBuildingInfos(); ++i) { const CvBuildingEntry* x = GC.getBuildingInfo((BuildingTypes)i); d.m_kBuildingTypes.Add(x ? x->GetType() : NULL); }
	for (int i = 0; i < GC.getNumPromotionInfos(); ++i) { const CvPromotionEntry* x = GC.getPromotionInfo((PromotionTypes)i); d.m_kUnitPromotions.Add(x ? x->GetType() : NULL); }
}

std::string Str(const CvWorldBuilderMapTypeDesc::String& s) { const char* p = s; return p ? p : ""; }

// The file name the game expects for a type the file names: empty when the game lacks it (the loader maps it to "none").
std::string Expected(const std::vector<std::string>& file, const CvWorldBuilderMapTypeDesc::TypeMap& game, uint index)
{
	if (index >= file.size() || file[index].empty()) return "";
	uint found = 0;
	return game.FindType(file[index].c_str(), found) ? file[index] : "";
}

struct Diff
{
	int count;
	std::string first;
	Diff() : count(0) {}
	void add(const std::string& what) { if (count++ == 0) first = what; }
	std::string text() const { return count ? Format("%d (first: %s)", count, first.c_str()) : std::string("0"); }
};

// One map through CvWorldBuilderMap::Load, compared with the RawMap of the same bytes.
struct ReadResult
{
	std::string path, line;
	bool ok;
	ReadResult() : ok(false) {}
};

ReadResult ReadOne(const gc_host* host, const std::string& path, const CvWorldBuilderMapTypeDesc& game)
{
	ReadResult r;
	r.path = path;
	RawMap raw;
	if (!ReadHostFile(host, path, raw.bytes)) { r.line = "cannot read the file"; return r; }
	const bool layout = raw.Parse();

	std::auto_ptr<CvWorldBuilderMap> m(new CvWorldBuilderMap());
	const std::wstring wpath = Wide(path);
	const double t0 = NowMs();
	const bool loaded = m->Load(wpath.c_str(), game, true);
	const double ms = NowMs() - t0;
	const uint preview = CvWorldBuilderMap::PreviewPlayableCivCount(wpath.c_str());

	Diff terrain, height, flags, feature, wonder, resource, other, scen, players, cities, units, teams;
	const uint plotCount = raw.PlotCount();
	int cityCount = 0, unitCount = 0;
	if (loaded && layout)
	{
		for (uint i = 0; i < plotCount; ++i)
		{
			const unsigned char* p = raw.At(raw.plots + (size_t)i * 8);
			const CvWorldBuilderMap::PlotMapData& d = m->GetPlotData(i);
			const std::string t = Expected(raw.terrain, game.m_kTerrainTypes, p[0]);
			if (Str(game.m_kTerrainTypes.Get(d.GetTerrainType())) != t && !(t.empty() && d.GetTerrainType() == 0xff)) terrain.add(Format("plot %u: %s for %s", i, Str(game.m_kTerrainTypes.Get(d.GetTerrainType())).c_str(), t.c_str()));
			if ((int)d.GetPlotHeight() != p[4]) height.add(Format("plot %u: %d for %d", i, (int)d.GetPlotHeight(), p[4]));
			if (d.GetFlags() != p[3]) flags.add(Format("plot %u: %02x for %02x", i, d.GetFlags(), p[3]));
			const std::string f = Expected(raw.feature, game.m_kFeatures, p[2]);
			if ((d.GetFeatureType() == 0xff ? std::string() : Str(game.m_kFeatures.Get(d.GetFeatureType()))) != f) feature.add(Format("plot %u: %u for %s", i, d.GetFeatureType(), f.c_str()));
			const std::string nw = Expected(raw.wonder, game.m_kNaturalWonders, p[6]);
			if ((d.GetNaturalWonderType() == 0xff ? std::string() : Str(game.m_kNaturalWonders.Get(d.GetNaturalWonderType()))) != nw) wonder.add(Format("plot %u: %u for %s", i, d.GetNaturalWonderType(), nw.c_str()));
			const std::string rs = Expected(raw.resource, game.m_kResources, p[1]);
			if ((d.GetResourceType() == 0xff ? std::string() : Str(game.m_kResources.Get(d.GetResourceType()))) != rs) resource.add(Format("plot %u: %u for %s", i, d.GetResourceType(), rs.c_str()));
			if (d.GetContinentType() != p[5] || d.GetResourceAmount() != p[7]) other.add(Format("plot %u: continent %u / amount %u for %u / %u", i, d.GetContinentType(), d.GetResourceAmount(), p[5], p[7]));
		}
		if (m->GetWidth() != raw.w || m->GetHeight() != raw.h || m->m_uiFlags != raw.flags || std::string(m->GetWorldType()) != raw.worldType)
			other.add(Format("map %ux%u flags %x world '%s' for %ux%u %x '%s'", m->GetWidth(), m->GetHeight(), m->m_uiFlags, m->GetWorldType(), raw.w, raw.h, raw.flags, raw.worldType.c_str()));
		if (raw.scenario)
		{
			if (m->GetPlayerCount() != raw.nPlayers || m->GetCityStateCount() != raw.nCityStates || m->GetTeamCount() != raw.nTeams)
				players.add(Format("counts %u/%u/%u for %u/%u/%u", m->GetPlayerCount(), m->GetCityStateCount(), m->GetTeamCount(), raw.nPlayers, raw.nCityStates, raw.nTeams));
			for (uint i = 0; i < raw.nPlayers && i < CvWorldBuilderMap::MaxPlayers; ++i)
				if (raw.PlayerCiv(i, false) != m->GetPlayer((byte)i).m_szCivType) players.add(Format("player %u: %s for %s", i, m->GetPlayer((byte)i).m_szCivType, raw.PlayerCiv(i, false).c_str()));
			for (uint i = 0; i < raw.nCityStates && i < CvWorldBuilderMap::MaxCityStates; ++i)
				if (raw.PlayerCiv(i, true) != m->GetCityState((byte)i).m_szCivType) players.add(Format("city-state %u: %s for %s", i, m->GetCityState((byte)i).m_szCivType, raw.PlayerCiv(i, true).c_str()));
			for (uint i = 0; i < raw.nTeams && i < CvWorldBuilderMap::MaxTeams; ++i)
				if (std::string((const char*)raw.At(raw.teams + i * 0x40), 32).c_str() != std::string(m->GetTeam((byte)i).m_szName)) teams.add(Format("team %u name", i));
			const std::vector<bool> usedCities = raw.UsedEntries(raw.cities, raw.citiesSize, raw.CityRecord());
			const std::vector<bool> usedUnits = raw.UsedEntries(raw.units, raw.unitsSize, raw.UnitRecord());
			for (uint i = 0; i < plotCount; ++i)
			{
				const unsigned char* s = raw.At(raw.scenPlots + (size_t)i * 8);
				const CvWorldBuilderMap::PlotScenarioData& d = m->GetPlotScenarioData(i);
				const std::string im = Expected(raw.impr, game.m_kImprovements, s[5]);
				if ((d.m_byImprovement == 0xff ? std::string() : Str(game.m_kImprovements.Get(d.m_byImprovement))) != im) scen.add(Format("plot %u: improvement %u for %s", i, d.m_byImprovement, im.c_str()));
				if (d.m_byCulture != s[4] || d.m_byRoute != s[6] || d.m_byRouteOwner != s[7]) scen.add(Format("plot %u: culture / route / owner", i));
				const uint hc = raw.U16(raw.scenPlots + (size_t)i * 8);
				const bool rawCity = hc < usedCities.size() && usedCities[hc];
				const CvWorldBuilderMap::City* c = m->m_kCities[d.m_hCity];
				if (rawCity)
				{
					++cityCount;
					const size_t rec = raw.cities + 4 + (size_t)hc * raw.CityRecord();
					const uint owner = raw.U8(rec + 0x40);
					const std::string name = std::string((const char*)raw.At(rec), raw.version >= 8 ? 64 : 56).c_str();
					if (!c || c->m_byOwner != owner || name != c->m_szName) cities.add(Format("plot %u: city %s", i, c ? c->m_szName : "missing"));
				}
				else if (c && hc < usedCities.size())
					cities.add(Format("plot %u: a city in a free entry", i));
				// Unit stacks: the same length and, per unit, the owner and the unit type name.
				uint hu = raw.U16(raw.scenPlots + (size_t)i * 8 + 2);
				const CvWorldBuilderMap::Unit* u = m->m_kUnits[d.m_hUnitStack];
				for (uint steps = 0; hu < usedUnits.size() && steps < 0x10000; ++steps)
				{
					const size_t rec = raw.units + 4 + (size_t)hu * raw.UnitRecord();
					const uint type = raw.version >= 12 ? raw.U32(rec + 0xc) : raw.version >= 8 ? raw.U8(rec + 0xc) : raw.U8(rec + 0x8);
					const uint owner = raw.version >= 12 ? raw.U8(rec + 0x10) : raw.version >= 8 ? raw.U8(rec + 0xd) : raw.U8(rec + 0x9);
					const std::string want = Expected(raw.unitTypes, game.m_kUnits, type);
					++unitCount;
					if (!u) { units.add(Format("plot %u: stack shorter", i)); break; }
					const std::string got = u->m_byUnitType == CvWorldBuilderMap::Unit::InvalidUnitType ? std::string() : Str(game.m_kUnits.Get(u->m_byUnitType));
					if (got != want || u->m_byOwner != owner) units.add(Format("plot %u: unit %s/%u for %s/%u", i, got.c_str(), u->m_byOwner, want.c_str(), owner));
					hu = raw.U16(rec);
					u = m->m_kUnits[u->m_hStackedUnit];
				}
				if (u) units.add(Format("plot %u: stack longer", i));
			}
		}
	}
	const uint wantPreview = raw.scenario && raw.version >= 5 ? raw.playable : 0;
	const int bad = terrain.count + height.count + flags.count + feature.count + wonder.count + resource.count + other.count + scen.count + players.count + cities.count +
		units.count + teams.count;
	r.ok = loaded && layout && bad == 0 && preview == wantPreview;
	r.line = Format("v%d%s %ux%u (%u plots) world '%s' flags %x; players %u cs %u teams %u, %d cities, %d units; load %s %.1f ms, layout %s%s%s; preview %u (header %u)",
		raw.version, raw.scenario ? " scenario" : "", raw.w, raw.h, plotCount, raw.worldType.c_str(), raw.flags, raw.nPlayers, raw.nCityStates, raw.nTeams, cityCount, unitCount,
		loaded ? "ok" : "FAILED", ms, layout ? "ok" : "BAD", layout ? "" : ": ", raw.error.c_str(), preview, wantPreview);
	if (bad)
		r.line += Format("; differences: terrain %s, height %s, flags %s, feature %s, wonder %s, resource %s, other %s, scenario plots %s, players %s, cities %s, units %s, teams %s",
			terrain.text().c_str(), height.text().c_str(), flags.text().c_str(), feature.text().c_str(), wonder.text().c_str(), resource.text().c_str(), other.text().c_str(),
			scen.text().c_str(), players.text().c_str(), cities.text().c_str(), units.text().c_str(), teams.text().c_str());
	return r;
}

}  // namespace

extern "C" int rt_selftest_wbmapread(const gc_host* host, const char* const* maps, int count, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c("wbmapread");
	RtSession* session = new RtSession();  // left alive, as in scenario `load`
	RtSession::LoadStatus ls;
	const bool loaded = session->Load(*host, ls);
	c.add("load", loaded, "boot reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());
	c.add("maps_given", count > 0, "%d map(s)", count);
	std::string mapsJson;
	int ok = 0;
	if (loaded)
	{
		CvWorldBuilderMapTypeDesc game;
		GameTypeDesc(game);
		for (int i = 0; i < count; ++i)
		{
			const ReadResult r = ReadOne(host, maps[i], game);
			ok += r.ok ? 1 : 0;
			c.add(r.path.c_str(), r.ok, "%s", r.line.c_str());
			mapsJson += std::string(mapsJson.empty() ? "" : ",") + "{\"path\":\"" + Escape(r.path) + "\",\"ok\":" + (r.ok ? "true" : "false") + ",\"detail\":\"" + Escape(r.line) + "\"}";
		}
		// A missing file and a damaged one fail cleanly.
		std::auto_ptr<CvWorldBuilderMap> m(new CvWorldBuilderMap());
		c.add("missing_file", !m->Load(L"maps/no/such.Civ5Map", game, true), "Load of a missing file returns false");
	}
	printf("wbmapread-summary|%d/%d map(s) loaded and equal to the file\n", ok, count);
	const std::string out = Format("{\"failed\":%d,\"maps_ok\":%d,\"maps\":%d,\"list\":[", c.failed, ok, count) + mapsJson + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}

// ---- scenario wbmap -------------------------------------------------------------------------------------
namespace {

// The game's map after the start against the file (CvWorldBuilderMapLoader::InitMap writes these).
std::string CompareGame(const RawMap& raw, const CvWorldBuilderMapTypeDesc& game, bool scenario, int& bad, int& mismatchedOwners, int& unitsFile, int& unitsGame)
{
	CvMap& map = GC.getMap();
	Diff terrain, height, feature, resource, rivers, improvement, route, cities, owner;
	const bool randomResources = (raw.flags & CvWorldBuilderMap::FLAG_RANDOM_RECOURCES) != 0;
	unitsFile = unitsGame = 0;
	std::vector<bool> usedCities, usedUnits;
	if (scenario)
	{
		usedCities = raw.UsedEntries(raw.cities, raw.citiesSize, raw.CityRecord());
		usedUnits = raw.UsedEntries(raw.units, raw.unitsSize, raw.UnitRecord());
	}
	for (uint i = 0; i < raw.PlotCount(); ++i)
	{
		const CvPlot* p = map.plotByIndex((int)i);
		if (!p) { terrain.add(Format("plot %u missing", i)); continue; }
		const unsigned char* d = raw.At(raw.plots + (size_t)i * 8);
		const std::string t = Expected(raw.terrain, game.m_kTerrainTypes, d[0]);
		const CvTerrainInfo* ti = GC.getTerrainInfo(p->getTerrainType());
		if ((ti ? ti->GetType() : "") != t) terrain.add(Format("(%d,%d) %s for %s", p->getX(), p->getY(), ti ? ti->GetType() : "?", t.c_str()));
		const PlotTypes want = d[4] == 1 ? PLOT_HILLS : d[4] == 2 ? PLOT_MOUNTAIN : (ti && ti->isWater()) ? PLOT_OCEAN : PLOT_LAND;
		if (p->getPlotType() != want) height.add(Format("(%d,%d) plot type %d for %d", p->getX(), p->getY(), (int)p->getPlotType(), (int)want));
		std::string f = Expected(raw.wonder, game.m_kNaturalWonders, d[6]);
		if (f.empty()) f = Expected(raw.feature, game.m_kFeatures, d[2]);
		const CvFeatureInfo* fi = p->getFeatureType() != NO_FEATURE ? GC.getFeatureInfo(p->getFeatureType()) : NULL;
		// A city removes the feature and the improvement of its plot and gets the city route (CvCity::init, CvPlot::updateCityRoute).
		const bool cityPlot = scenario && p->isCity();
		if (!cityPlot && (fi ? fi->GetType() : "") != f) feature.add(Format("(%d,%d) %s for %s", p->getX(), p->getY(), fi ? fi->GetType() : "none", f.c_str()));
		if (!randomResources)
		{
			const std::string rs = Expected(raw.resource, game.m_kResources, d[1]);
			const CvResourceInfo* ri = p->getResourceType(NO_TEAM) != NO_RESOURCE ? GC.getResourceInfo(p->getResourceType(NO_TEAM)) : NULL;
			if ((ri ? ri->GetType() : "") != rs) resource.add(Format("(%d,%d) %s for %s", p->getX(), p->getY(), ri ? ri->GetType() : "none", rs.c_str()));
		}
		const uint fl = d[3];
		const bool w = (fl & 0x01) != 0, nw = (fl & 0x02) != 0, ne = (fl & 0x04) != 0;
		bool rv = p->isWOfRiver() == w && p->isNWOfRiver() == nw && p->isNEOfRiver() == ne;
		if (w) rv = rv && p->getRiverEFlowDirection() == ((fl & 0x08) ? FLOWDIRECTION_SOUTH : FLOWDIRECTION_NORTH);
		if (nw) rv = rv && p->getRiverSEFlowDirection() == ((fl & 0x10) ? FLOWDIRECTION_NORTHEAST : FLOWDIRECTION_SOUTHWEST);
		if (ne) rv = rv && p->getRiverSWFlowDirection() == ((fl & 0x20) ? FLOWDIRECTION_NORTHWEST : FLOWDIRECTION_SOUTHEAST);
		if (!rv) rivers.add(Format("(%d,%d) flags %02x", p->getX(), p->getY(), fl));
		if (!scenario) continue;
		const unsigned char* s = raw.At(raw.scenPlots + (size_t)i * 8);
		const std::string im = Expected(raw.impr, game.m_kImprovements, s[5]);
		const CvImprovementEntry* ii = p->getImprovementType() != NO_IMPROVEMENT ? GC.getImprovementInfo(p->getImprovementType()) : NULL;
		if (!cityPlot && (ii ? ii->GetType() : "") != im) improvement.add(Format("(%d,%d) %s for %s", p->getX(), p->getY(), ii ? ii->GetType() : "none", im.c_str()));
		const RouteTypes wantRoute = s[6] == 0xff ? NO_ROUTE : s[6] == 0 ? ROUTE_ROAD : ROUTE_RAILROAD;
		if (!cityPlot && p->getRouteType() != wantRoute) route.add(Format("(%d,%d) %d for %d", p->getX(), p->getY(), (int)p->getRouteType(), (int)wantRoute));
		const uint hc = raw.U16(raw.scenPlots + (size_t)i * 8);
		const bool rawCity = hc < usedCities.size() && usedCities[hc];
		if (rawCity != p->isCity()) cities.add(Format("(%d,%d) city %d for %d", p->getX(), p->getY(), (int)p->isCity(), (int)rawCity));
		// Culture: a major player index < 32 -> that slot; city-states 32..95 -> 22 + n; anything else -> no owner (GetPlayerType).
		const uint cu = s[4];
		const PlayerTypes wantOwner = cu < 32 ? (cu < MAX_MAJOR_CIVS ? (PlayerTypes)cu : NO_PLAYER) : cu < 96 ? (cu - 32 < MAX_MINOR_CIVS ? (PlayerTypes)(cu - 32 + MAX_MAJOR_CIVS) : NO_PLAYER)
			: cu == 96 ? BARBARIAN_PLAYER : NO_PLAYER;
		if (p->getOwner() != wantOwner) owner.add(Format("(%d,%d) owner %d for %d", p->getX(), p->getY(), (int)p->getOwner(), (int)wantOwner));
		uint hu = raw.U16(raw.scenPlots + (size_t)i * 8 + 2);
		for (uint steps = 0; hu < usedUnits.size() && steps < 0x10000; ++steps)
		{
			++unitsFile;
			hu = raw.U16(raw.units + 4 + (size_t)hu * raw.UnitRecord());
		}
	}
	for (int i = 0; i < MAX_PLAYERS; ++i) unitsGame += GET_PLAYER((PlayerTypes)i).getNumUnits();
	mismatchedOwners = owner.count;
	bad = terrain.count + height.count + feature.count + resource.count + rivers.count + improvement.count + route.count + cities.count;
	return Format("terrain %s, plot type %s, feature %s, resource %s%s, rivers %s, improvement %s, route %s, cities %s, owners %s", terrain.text().c_str(),
		height.text().c_str(), feature.text().c_str(), resource.text().c_str(), randomResources ? " (random resources: not compared)" : "", rivers.text().c_str(),
		improvement.text().c_str(), route.text().c_str(), cities.text().c_str(), owner.text().c_str());
}

}  // namespace

extern "C" int rt_selftest_wbmap(const gc_host* host, const char* map, unsigned scenario, unsigned seed, unsigned turns, unsigned ai, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c("wbmap");
	const std::string path = map ? map : "";
	RawMap raw;
	const bool readable = !path.empty() && ReadHostFile(host, path, raw.bytes);
	const bool parsed = readable && raw.Parse();
	c.add("file", parsed, "%s: %s%s", path.c_str(), readable ? "read" : "not readable", parsed ? "" : Format(", %s", raw.error.c_str()).c_str());

	RtSession* session = new RtSession();  // left alive, as in scenario `load`
	RtSession::LoadStatus ls;
	const bool loaded = parsed && session->Load(*host, ls);
	c.add("load", loaded, "boot reached step %s%s%s", RtSession::StepName(ls.step), ls.error.empty() ? "" : ": ", ls.error.c_str());

	// The map's players in the slots (scenario), else one human and `ai` AI; the human is the test bot.
	const bool scen = scenario != 0;
	const uint slots = scen ? raw.nPlayers : 1 + ai;
	std::string players;
	for (uint i = 0; i < slots; ++i)
		players += Format("%s{\"kind\":\"%s\"%s}", i ? "," : "", i == 0 ? "human" : "ai", i == 0 ? ",\"nick\":\"wbbot\"" : "");
	const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"autoplay_humans\":[0],\"world_builder_map\":\"%s\",\"world_builder_scenario\":%u}",
		seed, players.c_str(), path.c_str(), scenario);
	RtSession::NewGameStatus st;
	const bool started = loaded && session->NewGame(*host, setup, st);
	c.add("new_game", started, "%s reached step %s (Init2 %.0f ms)%s%s", setup.c_str(), RtSession::NewGameStepName(st.step), st.msInit2, st.error.empty() ? "" : ": ", st.error.c_str());

	int startTurn = -1, doneTurns = 0, stuck = 0, luaErrors = 0, bad = -1, owners = 0, unitsFile = 0, unitsGame = 0;
	std::string stopReason, digestsJson, compare;
	uint64_t digest = 0;
	if (started)
	{
		CvMap& gm = GC.getMap();
		c.add("size", gm.getGridWidth() == (int)raw.w && gm.getGridHeight() == (int)raw.h && gm.isWrapX() == ((raw.flags & 1) != 0),
			"%d x %d wrapX=%d, the file %u x %u flags %x", gm.getGridWidth(), gm.getGridHeight(), (int)gm.isWrapX(), raw.w, raw.h, raw.flags);
		CvWorldBuilderMapTypeDesc game;
		GameTypeDesc(game);
		compare = CompareGame(raw, game, scen, bad, owners, unitsFile, unitsGame);
		c.add("plots_equal", bad == 0, "after the start: %s", compare.c_str());
		if (scen)
		{
			int civs = 0, alive = 0;
			std::string list;
			for (uint i = 0; i < raw.nPlayers && i < (uint)MAX_MAJOR_CIVS; ++i)
			{
				const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
				const CvCivilizationInfo* ci = GC.getCivilizationInfo(p.getCivilizationType());
				const std::string want = raw.PlayerCiv(i, false);
				civs += ci && want == ci->GetType() ? 1 : 0;
				alive += p.isAlive() ? 1 : 0;
				list += Format("%s%u:%s%s/%d cities", list.empty() ? "" : ", ", i, ci ? ci->GetType() : "?", p.isHuman() ? "(human)" : "", p.getNumCities());
			}
			c.add("scenario_players", civs == (int)raw.nPlayers && alive == (int)raw.nPlayers, "%d/%u civilizations as in the file, %d alive: %s", civs, raw.nPlayers, alive, list.c_str());
			c.add("scenario_units", unitsGame > 0, "%d unit(s) in the game, %d in the file's stacks", unitsGame, unitsFile);
		}

		RtScriptSystem* ss = session->Scripts();
		const int luaBefore = ss ? ss->GetErrorCount() : 0;
		std::vector<RtAutoplay> bots(MAX_PLAYERS);
		startTurn = GC.getGame().getGameTurn();
		int turn = startTurn, waitingSteps = 0;
		const int target = startTurn + (int)turns;
		const long long maxPumps = 200000LL * (turns + 1);
		long long pumps = 0;
		while (turn < target)
		{
			if (++pumps > maxPumps) { stopReason = Format("turn %d: %lld pumps without reaching turn %d", turn, maxPumps, target); break; }
			const RtSession::PumpResult r = session->Pump(64);
			if (r.status == RtSession::PUMP_STUCK) { ++stuck; stopReason = Format("STUCK at turn %d: %s", r.turn, r.reason.c_str()); break; }
			if (r.status == RtSession::PUMP_GAME_OVER) { stopReason = Format("GAME_OVER at turn %d", r.turn); break; }
			if (r.turn != turn)
			{
				const uint64_t d = RtSession::StateDigest();
				printf("wbmap-turn|%d|%016llx\n", r.turn, (unsigned long long)d);
				digestsJson += std::string(digestsJson.empty() ? "" : ",") + Format("\"%016llx\"", (unsigned long long)d);
				doneTurns += r.turn - turn;
				turn = r.turn;
				if (turn >= target) break;
			}
			if (r.status != RtSession::PUMP_WAITING_HUMAN) continue;
			++waitingSteps;
			const std::vector<PlayerTypes> waiting = session->WaitingHumans();
			for (size_t j = 0; j < waiting.size(); ++j)
			{
				const PlayerTypes who = waiting[(j + (size_t)waitingSteps) % waiting.size()];
				if (!session->IsHumanWaiting(who)) continue;
				std::string why;
				if (!bots[(int)who].ActAs(*session, (int)who, why)) { stopReason = Format("turn %d human %d: %s", turn, (int)who, why.c_str()); break; }
			}
			if (!stopReason.empty()) break;
		}
		luaErrors = ss ? ss->GetErrorCount() - luaBefore : 0;
		digest = RtSession::StateDigest();
	}
	c.add("turns", started && doneTurns == (int)turns && stopReason.empty(), "%d/%u turn(s) from turn %d%s%s", doneTurns, turns, startTurn, stopReason.empty() ? "" : ": ", stopReason.c_str());
	c.add("no_stuck", stuck == 0, "%d STUCK", stuck);
	c.add("lua_errors", started && luaErrors == 0 && st.luaErrorsMap == 0 && st.luaErrorsContexts == 0 && st.luaErrorsStart == 0, "%d during the turns; map %d, contexts %d, start %d",
		luaErrors, st.luaErrorsMap, st.luaErrorsContexts, st.luaErrorsStart);
	printf("wbmap-summary|%s|scenario %u|%ux%u|turns %d|digest %016llx\n", path.c_str(), scenario, raw.w, raw.h, doneTurns, (unsigned long long)digest);

	const std::string out = Format("{\"failed\":%d,\"map\":\"%s\",\"scenario\":%u,\"seed\":%u,\"width\":%u,\"height\":%u,\"version\":%d,\"start_turn\":%d,\"turns_done\":%d,"
		"\"plot_differences\":%d,\"owner_differences\":%d,\"units_file\":%d,\"units_game\":%d,\"state_digest\":\"%016llx\",\"digests\":[",
		c.failed, Escape(path).c_str(), scenario, seed, raw.w, raw.h, raw.version, startTurn, doneTurns, bad, owners, unitsFile, unitsGame, (unsigned long long)digest) +
		digestsJson + "],\"setup\":\"" + Escape(setup) + "\",\"compare\":\"" + Escape(compare) + "\",\"stop_reason\":\"" + Escape(stopReason) + "\",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
