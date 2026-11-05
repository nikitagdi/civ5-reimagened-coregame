// Q_PLOT_BASIS (19) and Q_PLOTS_DIRTY (21) (4.14, GH-R7, task N3.8). C++03.
// Q_PLOT_BASIS: the static terrain of the whole map, fog-free (NP-D19). Q_PLOTS_DIRTY: the plots whose Q_PLOTS / Q_PLOT_BASIS data may have changed for a watched team since
// the last call, found in the ABI layer by comparing a compact snapshot with the previous one; nothing in the core is hooked or changed.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "gamecore_abi.h"

class CvPlot;

namespace lekmod_portable {

// The river edge of `plot` towards DirectionType d (0 NE, 1 E, 2 SE, 3 SW, 4 W, 5 NW): the edges E, SE, SW come from the plot's own flags, W, NW, NE from the neighbour
// that owns them, so an edge reads the same from both of its plots (shared by Q_PLOTS, Q_PLOT_BASIS and the dirty snapshot).
bool PlotRiverEdge(const CvPlot* plot, int d);

// The job under way on a plot (P13, Q_PLOTS build_type / build_pct): the build with the most progress (progress * 100 / the build time for the plot's owner, capped at 100), or type -1 and pct 0.
// Shared by Q_PLOTS and the dirty snapshot.
void PlotBuildState(const CvPlot* plot, int16_t* type, uint8_t* pct);

// Q_PLOT_BASIS: args {} = the whole map by index, or {plot index, ...} = those plots in the order given (a bad index is GC_ERR_INVALID_ARG).
int32_t QueryPlotBasis(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg);

// The ABI layer's snapshot behind Q_PLOTS_DIRTY. One per gc_instance; it lives in the layer, is never saved and never touches the simulation. The first call (no snapshot) lists
// every plot, and so does the first call after the set of watched teams changed (a slot taken: that team's view needs a full keyframe) and a call in which the type or the
// landmass of any plot changed (LAKE and COASTAL are derived from them and a water body can cross MIN_WATER_SIZE_FOR_OCEAN).
class PlotDirtyTracker
{
public:
	PlotDirtyTracker();
	// Q_PLOTS_DIRTY: the changed plot indices ascending (gcq_plot_dirty records); resets the set, i.e. takes a new snapshot.
	int32_t Query(gc_buf* out, std::string& msg);
	// Forgets the snapshot: the next Query lists every plot.
	void Reset();
	// For the scenario: what the last Query did.
	int LastCount() const { return m_lastCount; }
	bool LastWasKeyframe() const { return m_lastKeyframe; }
	size_t SnapshotBytes() const;

	// Fields shared by every team's view of a plot. Only the fields Q_PLOTS and Q_PLOT_BASIS export: a field added to either reply is added here
	// in the same package (the `dirty` scenario catches a forgotten one).
	struct Common
	{
		int16_t terrain, feature, resource, improvement, route;
		int16_t landmass;
		int8_t plotType, resourceQty, owner, cityOwner, continentArt;
		uint8_t flags;      // 1 improvement pillaged, 2 route pillaged, 4 city on the plot, 8 worked by a city (P13)
		uint8_t riverBits;  // own edges: W of river (E edge), NW (SE edge), NE (SW edge)
		int8_t flow[3];     // E, SE, SW flow directions
		int8_t yields[6];
		int16_t buildType;  // P13: PlotBuildState
		uint8_t buildPct;
	};
	// One watched team's view of a plot (8 bytes).
	struct PerTeam
	{
		uint8_t flags;      // 1 revealed, 2 visible
		int8_t revealedOwner;
		int16_t revealedImprovement, revealedRoute, resource;
	};

private:
	std::vector<Common> m_common;
	std::vector<PerTeam> m_team;       // [team slot * plots + plot]
	std::vector<int> m_teams;          // the watched teams, ascending
	std::vector<Common> m_scratchCommon;
	std::vector<PerTeam> m_scratchTeam;
	bool m_valid;
	int m_width, m_height;
	int m_lastCount;
	bool m_lastKeyframe;
};

}  // namespace lekmod_portable
