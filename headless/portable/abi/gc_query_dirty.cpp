// Q_PLOT_BASIS and Q_PLOTS_DIRTY (task N3.8). See gc_query_dirty.h. C++03.
// Everything here only reads the game: the dirty snapshot is the ABI layer's own memory and no source of the core is hooked.
#include "CvGameCoreDLLPCH.h"

#include "CvGameCoreUtils.h"
#include "gc_gcq_writer.h"
#include "gc_query_dirty.h"

#include <algorithm>
#include <string.h>

namespace lekmod_portable {

namespace {

#define GCQ_ASSERT_SIZE(type, size) typedef char gcq_size_check_##type[(sizeof(type) == (size)) ? 1 : -1]
GCQ_ASSERT_SIZE(gcq_plot_basis, 16);
GCQ_ASSERT_SIZE(gcq_plot_dirty, 4);

int32_t Invalid(std::string& msg, const char* text)
{
	msg = text;
	return GC_ERR_INVALID_ARG;
}

int8_t Clamp8(int v) { return (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v)); }

// The terrain features Q_PLOT_BASIS keeps: natural wonders, marsh, flood plains, ice. Forest, jungle, oasis and fallout are not terrain relief; they stay in Q_PLOTS.
bool IsBasisFeature(int feature)
{
	static std::vector<signed char> cache;  // -1 unknown, 0 no, 1 yes
	if (feature < 0) return false;
	if (cache.empty()) cache.assign((size_t)GC.getNumFeatureInfos(), (signed char)-1);
	if (feature >= (int)cache.size()) return false;
	if (cache[feature] < 0)
	{
		const CvFeatureInfo* info = GC.getFeatureInfo((FeatureTypes)feature);
		const char* type = info ? info->GetType() : NULL;
		const bool keep = info && (info->IsNaturalWonder() || (type && (strcmp(type, "FEATURE_MARSH") == 0 || strcmp(type, "FEATURE_FLOOD_PLAINS") == 0 || strcmp(type, "FEATURE_ICE") == 0)));
		cache[feature] = keep ? 1 : 0;
	}
	return cache[feature] == 1;
}

}  // namespace

void PlotBuildState(const CvPlot* plot, int16_t* type, uint8_t* pct)
{
	*type = -1;
	*pct = 0;
	if (!plot->getAnyBuildProgress()) return;
	int best = -1, bestPct = -1;
	const int n = GC.getNumBuildInfos();
	for (int b = 0; b < n; ++b)
	{
		const int progress = plot->getBuildProgress((BuildTypes)b);
		if (progress <= 0) continue;
		const int time = plot->getBuildTime((BuildTypes)b, plot->getOwner());
		int p = time > 0 ? progress * 100 / time : 100;
		if (p > 100) p = 100;
		if (p > bestPct) { best = b; bestPct = p; }
	}
	if (best >= 0)
	{
		*type = (int16_t)best;
		*pct = (uint8_t)bestPct;
	}
}

namespace {

void FillCommon(const CvPlot* plot, PlotDirtyTracker::Common* c)
{
	memset(c, 0, sizeof(*c));
	c->plotType = (int8_t)plot->getPlotType();
	c->terrain = (int16_t)plot->getTerrainType();
	c->feature = (int16_t)plot->getFeatureType();
	c->resource = (int16_t)plot->getResourceType(NO_TEAM);
	c->resourceQty = c->resource >= 0 ? Clamp8(plot->getNumResource()) : 0;
	c->improvement = (int16_t)plot->getImprovementType();
	c->route = (int16_t)plot->getRouteType();
	c->owner = (int8_t)plot->getOwner();
	c->cityOwner = -1;
	unsigned flags = 0;
	if (plot->IsImprovementPillaged()) flags |= 1;
	if (plot->IsRoutePillaged()) flags |= 2;
	if (plot->isBeingWorked()) flags |= 8;
	if (plot->isCity())
	{
		flags |= 4;
		const CvCity* city = plot->getPlotCity();
		if (city) c->cityOwner = (int8_t)city->getOwner();
	}
	c->flags = (uint8_t)flags;
	c->riverBits = (uint8_t)((plot->isWOfRiver() ? 1 : 0) | (plot->isNWOfRiver() ? 2 : 0) | (plot->isNEOfRiver() ? 4 : 0));
	c->flow[0] = (int8_t)plot->getRiverEFlowDirection();
	c->flow[1] = (int8_t)plot->getRiverSEFlowDirection();
	c->flow[2] = (int8_t)plot->getRiverSWFlowDirection();
	c->continentArt = (int8_t)plot->GetContinentType();
	c->landmass = (int16_t)plot->getLandmass();
	for (int i = 0; i < 6; ++i) c->yields[i] = Clamp8(plot->getYield((YieldTypes)i));
	PlotBuildState(plot, &c->buildType, &c->buildPct);
}

void FillTeam(const CvPlot* plot, TeamTypes team, int rawResource, PlotDirtyTracker::PerTeam* t)
{
	memset(t, 0, sizeof(*t));
	t->revealedOwner = -1;
	t->revealedImprovement = -1;
	t->revealedRoute = -1;
	t->resource = -1;
	if (!plot->isRevealed(team, false)) return;
	unsigned flags = 1;
	if (plot->isVisible(team, false)) flags |= 2;
	t->flags = (uint8_t)flags;
	t->revealedOwner = (int8_t)plot->getRevealedOwner(team, false);
	t->revealedImprovement = (int16_t)plot->getRevealedImprovementType(team, false);
	t->revealedRoute = (int16_t)plot->getRevealedRouteType(team, false);
	if (rawResource >= 0) t->resource = (int16_t)plot->getResourceType(team);
}

// The teams of the seated humans, ascending: the teams whose views the host watches.
void WatchedTeams(std::vector<int>& out)
{
	out.clear();
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isHuman() || p.isObserver()) continue;
		const int t = (int)p.getTeam();
		if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
	}
	std::sort(out.begin(), out.end());
}

}  // namespace

bool PlotRiverEdge(const CvPlot* plot, int d)
{
	switch (d)
	{
	case DIRECTION_EAST: return plot->isWOfRiver();
	case DIRECTION_SOUTHEAST: return plot->isNWOfRiver();
	case DIRECTION_SOUTHWEST: return plot->isNEOfRiver();
	case DIRECTION_WEST:
	{
		const CvPlot* n = plotDirection(plot->getX(), plot->getY(), DIRECTION_WEST);
		return n && n->isWOfRiver();
	}
	case DIRECTION_NORTHWEST:
	{
		const CvPlot* n = plotDirection(plot->getX(), plot->getY(), DIRECTION_NORTHWEST);
		return n && n->isNWOfRiver();
	}
	case DIRECTION_NORTHEAST:
	{
		const CvPlot* n = plotDirection(plot->getX(), plot->getY(), DIRECTION_NORTHEAST);
		return n && n->isNEOfRiver();
	}
	}
	return false;
}

// ---- Q_PLOT_BASIS --------------------------------------------------------------------------------------------------

int32_t QueryPlotBasis(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg)
{
	CvMap& map = GC.getMap();
	const int n = map.numPlots();
	GcqBuf w(GC_Q_PLOT_BASIS, sizeof(gcq_plot_basis));
	const uint32_t count = argc == 0 ? (uint32_t)n : argc;
	w.Reserve(count);
	for (uint32_t k = 0; k < count; ++k)
	{
		const int idx = argc == 0 ? (int)k : args[k];
		if (idx < 0 || idx >= n) return Invalid(msg, "Q_PLOT_BASIS: plot index out of range");
		const CvPlot* plot = map.plotByIndex(idx);
		if (!plot) return Invalid(msg, "Q_PLOT_BASIS: no such plot");
		gcq_plot_basis r;
		memset(&r, 0, sizeof(r));
		r.index = (uint32_t)idx;
		r.plot_type = (int8_t)plot->getPlotType();
		r.continent_art = (uint8_t)plot->GetContinentType();
		r.terrain = (int16_t)plot->getTerrainType();
		const int feature = (int)plot->getFeatureType();
		r.feature = (int16_t)(IsBasisFeature(feature) ? feature : -1);
		unsigned flags = 0;
		if (plot->isLake()) flags |= GC_BASIS_F_LAKE;
		if (plot->isCoastalLand()) flags |= GC_BASIS_F_COASTAL;
		if (plot->IsNaturalWonder()) flags |= GC_BASIS_F_NATURAL_WONDER;
		r.flags = (uint16_t)flags;
		unsigned rivers = 0;
		for (int d = 0; d < NUM_DIRECTION_TYPES; ++d)
			if (PlotRiverEdge(plot, d)) rivers |= 1u << d;
		r.rivers = (uint8_t)rivers;
		r.river_flow[0] = (int8_t)plot->getRiverEFlowDirection();
		r.river_flow[1] = (int8_t)plot->getRiverSEFlowDirection();
		r.river_flow[2] = (int8_t)plot->getRiverSWFlowDirection();
		w.Add(&r);
	}
	if (!w.Finish(out))
	{
		msg = "out of memory";
		return GC_ERR_INTERNAL;
	}
	return GC_OK;
}

// ---- Q_PLOTS_DIRTY -------------------------------------------------------------------------------------------------

PlotDirtyTracker::PlotDirtyTracker() : m_valid(false), m_width(0), m_height(0), m_lastCount(0), m_lastKeyframe(false)
{
	GCQ_ASSERT_SIZE(PerTeam, 8);
}

void PlotDirtyTracker::Reset()
{
	m_valid = false;
	m_common.clear();
	m_team.clear();
	m_teams.clear();
}

size_t PlotDirtyTracker::SnapshotBytes() const { return m_common.size() * sizeof(Common) + m_team.size() * sizeof(PerTeam); }

int32_t PlotDirtyTracker::Query(gc_buf* out, std::string& msg)
{
	CvMap& map = GC.getMap();
	const int width = (int)map.getGridWidth(), height = (int)map.getGridHeight();
	const size_t n = (size_t)map.numPlots();
	std::vector<int> teams;
	WatchedTeams(teams);
	const size_t T = teams.size();
	const bool keyframe = !m_valid || m_width != width || m_height != height || m_teams != teams || m_common.size() != n;
	m_scratchCommon.resize(n);
	m_scratchTeam.resize(n * T);
	std::vector<unsigned char> dirty(n, 0);
	bool allDirty = keyframe;
	for (size_t i = 0; i < n; ++i)
	{
		const CvPlot* plot = map.plotByIndex((int)i);
		Common& c = m_scratchCommon[i];
		FillCommon(plot, &c);
		for (size_t t = 0; t < T; ++t) FillTeam(plot, (TeamTypes)teams[t], c.resource, &m_scratchTeam[t * n + i]);
		if (keyframe) continue;
		const Common& o = m_common[i];
		if (memcmp(&c, &o, sizeof(Common)) != 0)
		{
			dirty[i] = 1;
			// LAKE and COASTAL follow from the type and the landmass of the plots around; a water body can cross the ocean size threshold: every plot is dirty then.
			if (c.plotType != o.plotType || c.landmass != o.landmass) allDirty = true;
			// A river edge can belong to the neighbour (W, NW, NE of this plot): a changed edge or flow marks the plot and the six around it.
			if (c.riverBits != o.riverBits || memcmp(c.flow, o.flow, sizeof(c.flow)) != 0)
			{
				const int x = plot->getX(), y = plot->getY();
				for (int d = 0; d < NUM_DIRECTION_TYPES; ++d)
				{
					const CvPlot* nb = plotDirection(x, y, (DirectionTypes)d);
					if (nb) dirty[(size_t)nb->getY() * (size_t)width + (size_t)nb->getX()] = 1;
				}
			}
		}
		for (size_t t = 0; t < T; ++t)
			if (memcmp(&m_scratchTeam[t * n + i], &m_team[t * n + i], sizeof(PerTeam)) != 0) { dirty[i] = 1; break; }
	}
	GcqBuf w(GC_Q_PLOTS_DIRTY, sizeof(gcq_plot_dirty));
	int count = 0;
	for (size_t i = 0; i < n; ++i)
	{
		if (!allDirty && !dirty[i]) continue;
		gcq_plot_dirty r;
		r.index = (uint32_t)i;
		w.Add(&r);
		++count;
	}
	if (!w.Finish(out))
	{
		msg = "out of memory";
		return GC_ERR_INTERNAL;
	}
	m_common.swap(m_scratchCommon);
	m_team.swap(m_scratchTeam);
	m_teams = teams;
	m_width = width;
	m_height = height;
	m_valid = true;
	m_lastCount = count;
	m_lastKeyframe = allDirty;
	return GC_OK;
}

}  // namespace lekmod_portable
