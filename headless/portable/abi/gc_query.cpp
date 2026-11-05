// GCQ1 queries over the gamecore objects: Q_GAME, Q_PLOTS, Q_UNITS, Q_PLAYERS, Q_UNIT_ACTIONS,
// Q_PATH, Q_REACHABLE, Q_CATALOG.
// C++03 (the portable build uses -std=gnu++03). Everything here only reads the game; the buffer is malloc'ed and freed by
// gc_buf.release.
#include "CvGameCoreDLLPCH.h"

#include "CvGameCoreUtils.h"
#include "gc_command.h"
#include "gc_diplo.h"
#include "gc_notify.h"
#include "gc_query.h"
#include "gc_query_city.h"
#include "gc_query_dirty.h"
#include "gc_query_range.h"
#include "../runtime/rt_active.h"

#include <algorithm>
#include <map>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace lekmod_portable {

namespace {

#define GCQ_ASSERT_SIZE(type, size) typedef char gcq_size_check_##type[(sizeof(type) == (size)) ? 1 : -1]
GCQ_ASSERT_SIZE(gcq_header, 24);
GCQ_ASSERT_SIZE(gcq_game, 56);  // v2 (P1): the appended fields are zero until packages P4/P5 fill them
GCQ_ASSERT_SIZE(gcq_plot, 32);  // v2 (P13): + build_type, build_pct
GCQ_ASSERT_SIZE(gcq_player, 48);
GCQ_ASSERT_SIZE(gcq_catalog, 36);
GCQ_ASSERT_SIZE(gcq_unit, 44);  // v2 (P13): + transport_id, build_type, facing
GCQ_ASSERT_SIZE(gcq_unit_action, 8);
GCQ_ASSERT_SIZE(gcq_path_node, 12);
GCQ_ASSERT_SIZE(gcq_turn, 24);
GCQ_ASSERT_SIZE(gcq_diplo, 8);
GCQ_ASSERT_SIZE(gcq_tree_node, 16);
GCQ_ASSERT_SIZE(gcq_plot_basis, 16);
GCQ_ASSERT_SIZE(gcq_combat_preview, 20);
GCQ_ASSERT_SIZE(gcq_plot_dirty, 4);
GCQ_ASSERT_SIZE(gcq_catalog_timer, 24);
GCQ_ASSERT_SIZE(gc_error, 264);
GCQ_ASSERT_SIZE(gc_pump_result, 24);

// Collects fixed-size records and a deduplicated string table, then lays out one GCQ1 buffer.
class GcqWriter
{
public:
	GcqWriter(int32_t kind, uint32_t recordSize) : m_kind(kind), m_recordSize(recordSize), m_count(0) {}

	void Reserve(uint32_t records) { m_records.reserve((size_t)records * m_recordSize); }

	void Add(const void* record)
	{
		const uint8_t* p = static_cast<const uint8_t*>(record);
		m_records.insert(m_records.end(), p, p + m_recordSize);
		++m_count;
	}

	// Offset into the string table; NULL is "no string".
	uint32_t Str(const char* s)
	{
		if (!s) return GCQ_NO_STRING;
		std::map<std::string, uint32_t>::const_iterator it = m_pool.find(s);
		if (it != m_pool.end()) return it->second;
		const uint32_t off = (uint32_t)m_strings.size();
		m_strings.append(s, strlen(s) + 1);
		m_pool[s] = off;
		return off;
	}

	bool Finish(gc_buf* out)
	{
		const size_t total = sizeof(gcq_header) + m_records.size() + m_strings.size();
		uint8_t* data = static_cast<uint8_t*>(malloc(total > 0 ? total : 1));
		if (!data) return false;
		gcq_header h;
		memcpy(h.magic, "GCQ1", 4);
		h.format = 1;
		h.kind = (uint16_t)m_kind;
		h.count = m_count;
		h.record_size = m_recordSize;
		h.strings_offset = (uint32_t)(sizeof(gcq_header) + m_records.size());
		h.strings_size = (uint32_t)m_strings.size();
		memcpy(data, &h, sizeof(h));
		if (!m_records.empty()) memcpy(data + sizeof(h), &m_records[0], m_records.size());
		if (!m_strings.empty()) memcpy(data + h.strings_offset, m_strings.data(), m_strings.size());
		out->data = data;
		out->len = total;
		out->release = &Release;
		out->owner = NULL;
		return true;
	}

private:
	static void Release(gc_buf* b) { free(b->data); }

	int32_t m_kind;
	uint32_t m_recordSize;
	uint32_t m_count;
	std::vector<uint8_t> m_records;
	std::string m_strings;
	std::map<std::string, uint32_t> m_pool;
};

int32_t Invalid(std::string& msg, const char* text)
{
	msg = text;
	return GC_ERR_INVALID_ARG;
}

int32_t Internal(std::string& msg, const char* text)
{
	msg = text;
	return GC_ERR_INTERNAL;
}

// ---- Q_GAME ---------------------------------------------------------------------------------------------------

// Q_GAME (v2): args {} or {player}. Without args human_player is the active player in a single-type game (B1) and the first seated human in a
// network game (the server seat is not a human); with {player} the blocker and active_player describe that player.
int32_t QueryGame(const int32_t* args, uint32_t argc, int32_t lastPumpStatus, int32_t blocking, const RtSession::BlockTarget& target, const GameQueryExtra* extra, gc_buf* out, std::string& msg)
{
	CvGame& game = GC.getGame();
	CvMap& map = GC.getMap();
	RtSession* s = extra ? extra->session : NULL;
	const bool network = CvPreGame::isNetworkMultiplayerGame();
	gcq_game g;
	memset(&g, 0, sizeof(g));
	g.turn = game.getGameTurn();
	g.year = game.getGameTurnYear();
	g.blocker_unit = -1;
	g.blocker_city = -1;
	g.blocker_x = -1;
	g.blocker_y = -1;
	g.map_width = (int16_t)map.getGridWidth();
	g.map_height = (int16_t)map.getGridHeight();
	g.human_player = -1;
	g.human_team = -1;
	g.active_player = -1;
	g.blocker = -1;
	g.wrap_x = map.isWrapX() ? 1 : 0;
	g.wrap_y = map.isWrapY() ? 1 : 0;
	g.difficulty = (int8_t)game.getHandicapType();
	g.winner = (int8_t)game.getWinner();
	g.victory_type = (int8_t)game.getVictory();
	g.status = (int8_t)((game.getGameState() == GAMESTATE_OVER) ? GC_GAME_STATUS_GAME_OVER : (lastPumpStatus < 0 ? GC_GAME_STATUS_AI_RUNNING : lastPumpStatus));

	PlayerTypes human = game.getActivePlayer();
	const bool byArg = argc == 1;
	if (byArg)
	{
		if (args[0] < 0 || args[0] >= MAX_MAJOR_CIVS || !GET_PLAYER((PlayerTypes)args[0]).isAlive()) return Invalid(msg, "Q_GAME: player is not a living major civilization");
		human = (PlayerTypes)args[0];
	}
	else if (network)
	{
		// The active player of a network game is the server seat; B1 hosts read human_player as "the" human: the first seated one (a dead one when none lives).
		human = NO_PLAYER;
		for (int i = 0; i < MAX_MAJOR_CIVS && human == NO_PLAYER; ++i)
			if (IsSeatedHuman(i)) human = (PlayerTypes)i;
		for (int i = 0; i < MAX_MAJOR_CIVS && human == NO_PLAYER; ++i)
			if (CvPreGame::slotStatus((PlayerTypes)i) == SS_TAKEN) human = (PlayerTypes)i;
	}
	if (human >= 0 && human < MAX_PLAYERS)
	{
		CvPlayer& pl = GET_PLAYER(human);
		g.human_player = (int8_t)human;
		g.human_team = (int8_t)pl.getTeam();
		RtSession::BlockTarget tgt = target;
		int blk = blocking;
		if (s && (network || byArg)) blk = (pl.isAlive() && pl.isTurnActive()) ? s->CurrentBlocking(&tgt, human) : (int)NO_ENDTURN_BLOCKING_TYPE;
		g.active_player = (int8_t)((network || byArg) ? ((s && pl.isAlive() && pl.isTurnActive() && s->HumanPhase()) ? (int)human : -1) : (int)human);
		g.blocker = (int8_t)blk;
		if (blk >= 0)
		{
			g.blocker_x = (int16_t)tgt.x;
			g.blocker_y = (int16_t)tgt.y;
			if (tgt.kind == RtSession::TARGET_UNIT) g.blocker_unit = tgt.id;
			else if (tgt.kind == RtSession::TARGET_CITY) g.blocker_city = tgt.id;
		}
	}

	// v2 fields.
	g.game_type = (int8_t)CvPreGame::gameType();
	g.turn_mode = (int8_t)(network ? (game.isOption(GAMEOPTION_DYNAMIC_TURNS) ? GC_TURN_MODE_HYBRID : (game.isOption(GAMEOPTION_SIMULTANEOUS_TURNS) ? GC_TURN_MODE_SIMULTANEOUS : 0)) : 0);
	g.map_seed = CvPreGame::mapRandomSeed();
	if (s)
	{
		g.phase = (int8_t)(s->HumanPhase() ? GC_PHASE_HUMANS : GC_PHASE_AI);
		g.humans = (uint8_t)s->SeatedHumans();
		g.humans_left = (uint8_t)s->HumansLeft();
		// The turn timer: the brain has none (decision 3.8); these are the numbers the server's clock needs. turn_base_s is the base time of the chosen TurnTimers row.
		// turn_len_s is the length the original enforces for the turn that is running: Lekmod computes CvGame::getMaxTurnLen() once, when the turn starts
		// (GAME_UPDATE_TURN_TIMER_ONCE_PER_TURN, CvGame.cpp:8656) and tests the clock against that (getPreviousTurnLen, CvGame.cpp:2158); the original runs no timer in
		// the first turn (getElapsedGameTurns() > 0, CvGame.cpp:2121). 0 = no timer.
		if (s->Setup().Network() && s->Setup().turn_timer != "none")
		{
			g.turn_base_s = CvPreGame::turnTimerInfo().getBaseTime();
#ifdef GAME_UPDATE_TURN_TIMER_ONCE_PER_TURN
			g.turn_len_s = game.getElapsedGameTurns() > 0 ? (int32_t)(game.getPreviousTurnLen() + 0.5f) : 0;
#else
			g.turn_len_s = game.getElapsedGameTurns() > 0 ? (int32_t)game.getMaxTurnLen() : 0;
#endif
		}
	}
	g.unsafe_reason = (int8_t)(extra ? extra->unsafeReason : GC_UNSAFE_NONE);
	g.safe_point = (int8_t)((extra && extra->unsafeReason == GC_UNSAFE_NONE) ? 1 : 0);

	GcqWriter w(GC_Q_GAME, sizeof(g));
	w.Add(&g);
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

// ---- Q_TURN ---------------------------------------------------------------------------------------------------

// Q_TURN (v2): one record per slot that is or was a human, by player id; the status of everybody's turn and the input of the server's timer.
// turn_order: in a hybrid turn the humans that do not move simultaneously (at war with a human) are activated one after the other in player id order (CvGame::
// CheckPlayerTurnDeactivate); their rank in that order, 0 = first; -1 for a human who moves simultaneously and in every other mode.
int32_t QueryTurn(const GameQueryExtra* extra, gc_buf* out, std::string& msg)
{
	RtSession* s = extra ? extra->session : NULL;
	if (!s) return Internal(msg, "Q_TURN needs the runtime session");
	RtEngine& e = s->Engine();
	const bool hybrid = CvPreGame::isNetworkMultiplayerGame() && GC.getGame().isOption(GAMEOPTION_DYNAMIC_TURNS);
	const bool phase = s->HumanPhase();
	GcqWriter w(GC_Q_TURN, sizeof(gcq_turn));
	int rank = 0;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		if (!s->EverHuman(i)) continue;
		const PlayerTypes who = (PlayerTypes)i;
		const CvPlayer& p = GET_PLAYER(who);
		const bool human = IsSeatedHuman(i);
		gcq_turn r;
		memset(&r, 0, sizeof(r));
		r.player = (int8_t)i;
		r.team = (int8_t)p.getTeam();
		r.slot_status = (int8_t)CvPreGame::slotStatus(who);
		r.blocker = -1;
		r.turn_order = -1;
		r.blocker_unit = -1;
		r.blocker_city = -1;
		r.blocker_x = -1;
		r.blocker_y = -1;
		r.nick = GCQ_NO_STRING;
		unsigned flags = 0;
		ActiveAs as(who, human);  // audit F-06: the getters with a local-player branch answer for this human
		if (CvPreGame::slotStatus(who) == SS_TAKEN) flags |= GC_TURN_F_HUMAN;
		if (p.isAlive()) flags |= GC_TURN_F_ALIVE;
		// P8 (N3.12): the connection and the kick are the slot control's (RtEngine::SlotConnected / SlotKicked); DISCONNECTED is Lekmod's autopause flag of a human who dropped
		// in a turn that waits for it (CvPlayer::disconnected, CvPlayer.cpp:31255-31273).
		if (e.SlotConnected(who)) flags |= GC_TURN_F_CONNECTED;
		if (CvPreGame::slotStatus(who) == SS_TAKEN && p.isDisconnected()) flags |= GC_TURN_F_DISCONNECTED;
		if (CvPreGame::slotStatus(who) != SS_TAKEN && e.SlotKicked(who)) flags |= GC_TURN_F_KICKED;
		if (human)
		{
			if (p.isTurnActive()) flags |= GC_TURN_F_TURN_ACTIVE;
			if (e.TurnCompleteSent(who)) flags |= GC_TURN_F_TURN_ENDED;
			if (p.isTurnActive() && s->UI().CanEndTurnOf(who)) flags |= GC_TURN_F_CAN_END_TURN;
			if (p.isSimultaneousTurns()) flags |= GC_TURN_F_SIMULTANEOUS;
			if (p.isAutoMoves()) flags |= GC_TURN_F_AUTOMOVES;
			if (phase && !e.TurnCompleteSent(who)) flags |= GC_TURN_F_WAITING_FOR;
			if (hybrid && !p.isSimultaneousTurns()) r.turn_order = (int16_t)rank++;
			if (p.isTurnActive())
			{
				RtSession::BlockTarget t;
				r.blocker = (int8_t)s->CurrentBlocking(&t, who);
				if (r.blocker >= 0)
				{
					r.blocker_x = (int16_t)t.x;
					r.blocker_y = (int16_t)t.y;
					if (t.kind == RtSession::TARGET_UNIT) r.blocker_unit = t.id;
					else if (t.kind == RtSession::TARGET_CITY) r.blocker_city = t.id;
				}
			}
		}
		r.flags = (uint16_t)flags;
		const CvString& nick = CvPreGame::nickname(who);
		if (nick.c_str()[0]) r.nick = w.Str(nick.c_str());
		w.Add(&r);
	}
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

// ---- Q_PLOTS --------------------------------------------------------------------------------------------------

int8_t Clamp8(int v) { return (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v)); }

// Q_PLOTS: indices NULL = the whole map, rows by y then x; else (v2) only those plots (index = y * width + x), in the order given; a bad index is GC_ERR_INVALID_ARG.
int32_t QueryPlots(int32_t viewer, const int32_t* indices, uint32_t indexCount, gc_buf* out, std::string& msg)
{
	CvMap& map = GC.getMap();
	const int width = (int)map.getGridWidth();
	const int height = (int)map.getGridHeight();
	const bool all = viewer < 0;
	const TeamTypes team = all ? NO_TEAM : (TeamTypes)viewer;
	GcqWriter w(GC_Q_PLOTS, sizeof(gcq_plot));
	const uint32_t count = indices ? indexCount : (uint32_t)(width * height);
	w.Reserve(count);
	for (uint32_t k = 0; k < count; ++k)
	{
		{
			int x, y;
			if (indices)
			{
				if (indices[k] < 0 || indices[k] >= width * height) return Invalid(msg, "Q_PLOTS: plot index out of range");
				x = indices[k] % width;
				y = indices[k] / width;
			}
			else
			{
				x = (int)(k % (uint32_t)width);
				y = (int)(k / (uint32_t)width);
			}
			gcq_plot r;
			memset(&r, 0, sizeof(r));
			r.x = (int16_t)x;
			r.y = (int16_t)y;
			const CvPlot* plot = map.plot(x, y);
			const bool revealed = plot && (all || plot->isRevealed(team, false));
			if (!revealed)
			{
				w.Add(&r);
				continue;
			}
			const bool visible = all || plot->isVisible(team, false);
			unsigned flags = GC_PLOT_F_REVEALED;
			if (visible) flags |= GC_PLOT_F_VISIBLE;
			if (plot->isLake()) flags |= GC_PLOT_F_LAKE;
			if (plot->isCoastalLand()) flags |= GC_PLOT_F_COASTAL;
			if (plot->IsNaturalWonder()) flags |= GC_PLOT_F_NATURAL_WONDER;
			for (int d = 0; d < NUM_DIRECTION_TYPES; ++d)
				if (PlotRiverEdge(plot, d)) flags |= 1u << (GC_PLOT_F_RIVER_SHIFT + d);

			const int owner = all ? (int)plot->getOwner() : (int)plot->getRevealedOwner(team, false);
			const int improvement = all ? (int)plot->getImprovementType() : (int)plot->getRevealedImprovementType(team, false);
			const int route = all ? (int)plot->getRouteType() : (int)plot->getRevealedRouteType(team, false);
			const int resource = all ? (int)plot->getResourceType() : (int)plot->getResourceType(team);
			// A city on a fogged plot is only shown when the owner we last saw is the city's.
			bool city = false;
			if (plot->isCity())
			{
				const CvCity* c = plot->getPlotCity();
				city = visible || (c && owner >= 0 && (int)c->getOwner() == owner);
			}
			if (city) flags |= GC_PLOT_F_CITY;
			if (visible)
			{
				if (plot->IsImprovementPillaged()) flags |= GC_PLOT_F_PILLAGED;
				if (plot->IsRoutePillaged()) flags |= GC_PLOT_F_ROUTE_PILLAGED;
				for (int i = 0; i < 6; ++i) r.yields[i] = Clamp8(plot->getYield((YieldTypes)i));
			}
			// P13: a plot a city works shows to the viewer who sees it now and owns it (viewer -1: any worked plot).
			if (plot->isBeingWorked() && (all || (visible && owner >= 0 && (int)GET_PLAYER((PlayerTypes)owner).getTeam() == viewer))) flags |= GC_PLOT_F_WORKED;
			r.build_type = -1;
			if (visible) PlotBuildState(plot, &r.build_type, &r.build_pct);
			r.flags = (uint16_t)flags;
			r.plot_type = (int8_t)plot->getPlotType();
			r.terrain = (int16_t)plot->getTerrainType();
			r.feature = (int16_t)plot->getFeatureType();
			r.resource = (int16_t)resource;
			r.resource_qty = resource >= 0 ? Clamp8(plot->getNumResource()) : 0;
			r.improvement = (int16_t)improvement;
			r.route = (int16_t)route;
			r.owner = (int8_t)owner;
			r.continent_art = (int8_t)plot->GetContinentType();
			w.Add(&r);
		}
	}
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

// ---- Q_PLAYERS ------------------------------------------------------------------------------------------------

uint32_t PackColor(int colorType)
{
	const CvColorInfo* info = colorType >= 0 ? GC.GetColorInfo((ColorTypes)colorType) : NULL;
	if (!info) return 0;
	const CvColorA& c = info->GetColor();
	const unsigned r = (unsigned)(c.r * 255.0f + 0.5f), g = (unsigned)(c.g * 255.0f + 0.5f), b = (unsigned)(c.b * 255.0f + 0.5f), a = (unsigned)(c.a * 255.0f + 0.5f);
	return ((r & 255u) << 24) | ((g & 255u) << 16) | ((b & 255u) << 8) | (a & 255u);
}

// The technologies the landmark art depends on (ArtDefine_Landmarks.Tech, SELECT DISTINCT, by name): the bit order of gcq_player.art_techs and the rows of Q_CATALOG table 16 (at most 64).
const std::vector<int>& ArtTechList()
{
	static std::vector<int> list;
	static bool loaded = false;
	if (!loaded)
	{
		loaded = true;
		Database::Results rows;
		if (GC.GetGameDatabase()->Execute(rows, "SELECT DISTINCT \"Tech\" FROM \"ArtDefine_Landmarks\" WHERE \"Tech\" IS NOT NULL ORDER BY \"Tech\""))
			while (rows.Step() && list.size() < 64)
			{
				const char* type = rows.GetText(0);
				const int tech = type ? (int)GC.getInfoTypeForString(type, true) : -1;
				if (tech >= 0) list.push_back(tech);
			}
	}
	return list;
}

int32_t QueryPlayers(int32_t viewer, gc_buf* out, std::string& msg)
{
	const bool all = viewer < 0;
	GcqWriter w(GC_Q_PLAYERS, sizeof(gcq_player));
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		const CvPlayer& pl = GET_PLAYER((PlayerTypes)i);
		if (!pl.isEverAlive()) continue;
		const TeamTypes pteam = pl.getTeam();
		const bool own = !all && (int)pteam == viewer;
		const bool met = all || own || GET_TEAM((TeamTypes)viewer).isHasMet(pteam);
		gcq_player r;
		memset(&r, 0, sizeof(r));
		r.id = (int8_t)i;
		r.team = (int8_t)pteam;
			r.nick = GCQ_NO_STRING;  // v2 (P1): the nick and the slot status come with package P2
		unsigned flags = 0;
		if (pl.isAlive()) flags |= GC_PLAYER_F_ALIVE;
		if (pl.isMinorCiv()) flags |= GC_PLAYER_F_MINOR;
		if (pl.isBarbarian()) flags |= GC_PLAYER_F_BARBARIAN;
		const PlayerColorTypes colorType = pl.getPlayerColor();
		const CvPlayerColorInfo* colors = colorType >= 0 ? GC.GetPlayerColorInfo(colorType) : NULL;
		if (colors)
		{
			r.color_primary = PackColor(colors->GetColorTypePrimary());
			r.color_secondary = PackColor(colors->GetColorTypeSecondary());
		}
		if (met)
		{
			flags |= GC_PLAYER_F_MET;
			if (pl.isHuman()) flags |= GC_PLAYER_F_HUMAN;
			if (!all && !own && GET_TEAM((TeamTypes)viewer).isAtWar(pteam)) flags |= GC_PLAYER_F_AT_WAR_WITH_VIEWER;
			r.civ_type = (int16_t)pl.getCivilizationType();
			r.leader_type = (int16_t)pl.getLeaderType();
			r.name = w.Str(pl.getName());
			r.civ_name = w.Str(pl.getCivilizationShortDescription());
			const std::vector<int>& art = ArtTechList();
			for (size_t b = 0; b < art.size(); ++b)
				if (GET_TEAM(pteam).GetTeamTechs()->HasTech((TechTypes)art[b])) r.art_techs |= (uint64_t)1 << b;
		}
		else
		{
			r.civ_type = -1;
			r.leader_type = -1;
			r.era = -1;
			r.name = w.Str("Unknown");
			r.civ_name = w.Str("Unknown");
		}
		r.flags = (uint16_t)flags;
		w.Add(&r);
	}
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

// ---- Q_CATALOG ------------------------------------------------------------------------------------------------

struct CatalogEntry
{
	int id;
	std::string type;
	std::string name;
};

struct CatalogArt
{
	std::string artDefine, iconAtlas, flagAtlas, artStyle;
	bool hasArt, hasIcon, hasFlag, hasStyle;
	int iconIndex, flagIndex;
	CatalogArt() : hasArt(false), hasIcon(false), hasFlag(false), hasStyle(false), iconIndex(-1), flagIndex(-1) {}
};

// A Description column read straight from the game DB is a text key: the lobby lists (turn timers, world sizes,
// game speeds) carry the localized text, like the names of CollectInfos.
std::string LocalizedName(const char* key)
{
	if (!key) return std::string();
	if (strncmp(key, "TXT_KEY_", 8) != 0) return std::string(key);
	return std::string(GetLocalizedText(key).GetCString());
}

template <class V>
void CollectInfos(V& infos, std::vector<CatalogEntry>& out)
{
	for (size_t i = 0; i < infos.size(); ++i)
	{
		if (!infos[i]) continue;
		CatalogEntry e;
		e.id = (int)i;
		const char* type = infos[i]->GetType();
		const char* name = infos[i]->GetDescription();
		e.type = type ? type : "";
		e.name = name ? name : "";
		out.push_back(e);
	}
}

// Database columns that carry the asset references of one table ("NULL" = the table has no such column).
struct TableDef
{
	const char* table;
	const char* art;
	const char* style;
	const char* atlas;
	const char* index;
	const char* flagAtlas;
	const char* flagIndex;
};

const TableDef kTables[GC_CATALOG_TABLE_COUNT] = {
	{"Units", "UnitArtInfo", "NULL", "IconAtlas", "PortraitIndex", "UnitFlagAtlas", "UnitFlagIconOffset"},
	{"Buildings", "ArtDefineTag", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Projects", "NULL", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Technologies", "NULL", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Policies", "NULL", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Terrains", "ArtDefineTag", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Features", "ArtDefineTag", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Resources", "ArtDefineTag", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Improvements", "ArtDefineTag", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Routes", "NULL", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Civilizations", "ArtDefineTag", "ArtStyleType", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Leaders", "ArtDefineTag", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"Eras", "ArtPrefix", "CityArtType", "NULL", "NULL", "NULL", "NULL"},
	{"HandicapInfos", "NULL", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
	{"UnitPromotions", "NULL", "NULL", "IconAtlas", "PortraitIndex", "NULL", "NULL"},
};

std::string Col(const char* name)
{
	return strcmp(name, "NULL") == 0 ? std::string("NULL") : std::string("\"") + name + "\"";
}

void LoadArt(const TableDef& def, std::map<std::string, CatalogArt>& art)
{
	std::string sql = "SELECT \"Type\", " + Col(def.art) + ", " + Col(def.style) + ", " + Col(def.atlas) + ", " + Col(def.index) + ", " + Col(def.flagAtlas) + ", " +
		Col(def.flagIndex) + " FROM \"" + def.table + "\"";
	Database::Results rows;
	if (!GC.GetGameDatabase()->Execute(rows, sql.c_str())) return;
	while (rows.Step())
	{
		const char* type = rows.GetText(0);
		if (!type) continue;
		CatalogArt a;
		const char* artDefine = rows.GetText(1);
		const char* style = rows.GetText(2);
		const char* atlas = rows.GetText(3);
		const char* flagAtlas = rows.GetText(5);
		if (artDefine) { a.hasArt = true; a.artDefine = artDefine; }
		if (style) { a.hasStyle = true; a.artStyle = style; }
		if (atlas) { a.hasIcon = true; a.iconAtlas = atlas; a.iconIndex = rows.GetInt(4); }
		if (flagAtlas) { a.hasFlag = true; a.flagAtlas = flagAtlas; a.flagIndex = rows.GetInt(6); }
		art[type] = a;
	}
}

int32_t QueryCatalog(int32_t table, gc_buf* out, std::string& msg)
{
	std::vector<CatalogEntry> entries;
	if (table == GC_CATALOG_TURN_TIMERS)
	{
		// v2: the TurnTimers rows by ID (record gcq_catalog_timer); LobbyChoices.timers and Setup.turn_timer name a row by its type.
		GcqWriter tw(GC_Q_CATALOG, sizeof(gcq_catalog_timer));
		Database::Results rows;
		if (GC.GetGameDatabase()->Execute(rows, "SELECT \"ID\", \"Type\", \"Description\", \"BaseTime\", \"CityResource\", \"UnitResource\" FROM \"TurnTimers\" ORDER BY \"ID\""))
			while (rows.Step())
			{
				const char* type = rows.GetText(1);
				const std::string name = LocalizedName(rows.GetText(2));
				gcq_catalog_timer r;
				memset(&r, 0, sizeof(r));
				r.id = (int16_t)rows.GetInt(0);
				r.type = tw.Str(type ? type : "");
				r.name = tw.Str(name.c_str());
				r.base_time = rows.GetInt(3);
				r.city_resource = rows.GetInt(4);
				r.unit_resource = rows.GetInt(5);
				tw.Add(&r);
			}
		return tw.Finish(out) ? GC_OK : Internal(msg, "out of memory");
	}
	if (table == GC_CATALOG_WORLD_SIZES)
	{
		// v2 (lobby lists): the Worlds rows by ID (record gcq_catalog_world_size); LobbyChoices.world_sizes names a row by its type.
		GcqWriter ww(GC_Q_CATALOG, sizeof(gcq_catalog_world_size));
		Database::Results rows;
		if (GC.GetGameDatabase()->Execute(rows, "SELECT \"ID\", \"Type\", \"Description\", \"GridWidth\", \"GridHeight\", \"DefaultPlayers\", \"DefaultMinorCivs\" FROM \"Worlds\" ORDER BY \"ID\""))
			while (rows.Step())
			{
				const char* type = rows.GetText(1);
				const std::string name = LocalizedName(rows.GetText(2));
				gcq_catalog_world_size r;
				memset(&r, 0, sizeof(r));
				r.id = (int16_t)rows.GetInt(0);
				r.type = ww.Str(type ? type : "");
				r.name = ww.Str(name.c_str());
				r.width = (int16_t)rows.GetInt(3);
				r.height = (int16_t)rows.GetInt(4);
				r.default_players = (int16_t)rows.GetInt(5);
				r.default_minor = (int16_t)rows.GetInt(6);
				ww.Add(&r);
			}
		return ww.Finish(out) ? GC_OK : Internal(msg, "out of memory");
	}
	if (table == GC_CATALOG_GAME_SPEEDS)
	{
		// v2 (lobby lists): the GameSpeeds rows by ID; only id, type and name are set.
		GcqWriter sw(GC_Q_CATALOG, sizeof(gcq_catalog));
		Database::Results rows;
		if (GC.GetGameDatabase()->Execute(rows, "SELECT \"ID\", \"Type\", \"Description\" FROM \"GameSpeeds\" ORDER BY \"ID\""))
			while (rows.Step())
			{
				const char* type = rows.GetText(1);
				const std::string name = LocalizedName(rows.GetText(2));
				gcq_catalog r;
				memset(&r, 0, sizeof(r));
				r.id = (int16_t)rows.GetInt(0);
				r.icon_index = -1;
				r.flag_index = -1;
				r.type = sw.Str(type ? type : "");
				r.name = sw.Str(name.c_str());
				r.art_define = r.icon_atlas = r.flag_atlas = r.art_style = r.strategic_view = GCQ_NO_STRING;
				sw.Add(&r);
			}
		return sw.Finish(out) ? GC_OK : Internal(msg, "out of memory");
	}
	if (table == GC_CATALOG_ART_TECHS)
	{
		// v2 (P13): the landmark art technologies in the bit order of gcq_player.art_techs; only id, type and name are set.
		const std::vector<int>& art = ArtTechList();
		GcqWriter aw(GC_Q_CATALOG, sizeof(gcq_catalog));
		for (size_t i = 0; i < art.size(); ++i)
		{
			const CvTechEntry* info = GC.getTechInfo((TechTypes)art[i]);
			gcq_catalog r;
			memset(&r, 0, sizeof(r));
			r.id = (int16_t)art[i];
			r.icon_index = -1;
			r.flag_index = -1;
			r.type = aw.Str(info && info->GetType() ? info->GetType() : "");
			r.name = aw.Str(info && info->GetDescription() ? info->GetDescription() : "");
			r.art_define = r.icon_atlas = r.flag_atlas = r.art_style = r.strategic_view = GCQ_NO_STRING;
			aw.Add(&r);
		}
		return aw.Finish(out) ? GC_OK : Internal(msg, "out of memory");
	}
	if (table == GC_CATALOG_CIV_INFO)
	{
		// v4 (b9 input-lobby): the hover tip of a civilization in the staging room (leader, traits, unique units and buildings), straight DB reads.
		// SIMPLIFIED(SIMP-exe-961): the first leader of the civilization, not the leader the player picks (the lobby picks a civilization only).
		GcqWriter cw(GC_Q_CATALOG, sizeof(gcq_catalog_ext));
		Database::Results civs;
		if (GC.GetGameDatabase()->Execute(civs, "SELECT \"ID\", \"Type\" FROM \"Civilizations\" ORDER BY \"ID\""))
			while (civs.Step())
			{
				const char* ctype = civs.GetText(1);
				std::string safe;
				for (const char* p = ctype ? ctype : ""; *p; ++p) { if (*p == '\'') safe += '\''; safe += *p; }
				std::string leader, leaderType, traits, unique;
				{
					Database::Results rows;
					const std::string sql = "SELECT l.\"Type\", l.\"Description\" FROM \"Civilization_Leaders\" cl INNER JOIN \"Leaders\" l ON l.\"Type\" = cl.\"LeaderheadType\" WHERE cl.\"CivilizationType\" = '" + safe + "' ORDER BY cl.rowid LIMIT 1";
					if (GC.GetGameDatabase()->Execute(rows, sql.c_str()) && rows.Step())
					{
						if (rows.GetText(0)) leaderType = rows.GetText(0);
						leader = LocalizedName(rows.GetText(1));
					}
				}
				if (!leaderType.empty())
				{
					std::string ls;
					for (size_t i = 0; i < leaderType.size(); ++i) { if (leaderType[i] == '\'') ls += '\''; ls += leaderType[i]; }
					Database::Results rows;
					const std::string sql = "SELECT t.\"ShortDescription\", t.\"Description\" FROM \"Leader_Traits\" lt INNER JOIN \"Traits\" t ON t.\"Type\" = lt.\"TraitType\" WHERE lt.\"LeaderType\" = '" + ls + "' ORDER BY lt.rowid";
					if (GC.GetGameDatabase()->Execute(rows, sql.c_str()))
						while (rows.Step())
						{
							if (!traits.empty()) traits += "\n";
							traits += LocalizedName(rows.GetText(0)) + ": " + LocalizedName(rows.GetText(1));
						}
				}
				{
					Database::Results rows;
					const std::string sql = "SELECT 'Unit', u.\"Description\" FROM \"Civilization_UnitClassOverrides\" o INNER JOIN \"Units\" u ON u.\"Type\" = o.\"UnitType\" WHERE o.\"CivilizationType\" = '" + safe + "' UNION ALL SELECT 'Building', b.\"Description\" FROM \"Civilization_BuildingClassOverrides\" o INNER JOIN \"Buildings\" b ON b.\"Type\" = o.\"BuildingType\" WHERE o.\"CivilizationType\" = '" + safe + "'";
					if (GC.GetGameDatabase()->Execute(rows, sql.c_str()))
						while (rows.Step())
						{
							if (!unique.empty()) unique += "\n";
							unique += std::string(rows.GetText(0)) + ": " + LocalizedName(rows.GetText(1));
						}
				}
				gcq_catalog_ext r;
				memset(&r, 0, sizeof(r));
				r.n[0] = civs.GetInt(0);
				r.s[0] = cw.Str(ctype ? ctype : "");
				r.s[1] = cw.Str(leader.c_str());
				r.s[2] = cw.Str(traits.c_str());
				r.s[3] = cw.Str(unique.c_str());
				cw.Add(&r);
			}
		return cw.Finish(out) ? GC_OK : Internal(msg, "out of memory");
	}
	if (table >= GC_CATALOG_GAME_OPTIONS && table < GC_CATALOG_TABLE_COUNT_V3)
	{
		// v3 (login-lobby P2): the staging room options of the game data as gcq_catalog_ext rows (gamecore_abi.h). Straight DB reads, valid before a game.
		// SIMPLIFIED(SIMP-exe-850): the map scripts' custom options are the MapScriptOptions rows the exe stored from GetMapScriptInfo(), not a run of each script.
		struct V3Table { const char* sql; int mode; };
		static const V3Table kV3[] = {
			{"SELECT \"ID\", \"Type\", \"Description\", \"Help\", \"Default\", \"Visible\", \"SupportsMultiplayer\" FROM \"GameOptions\" ORDER BY \"ID\"", 0},
			{"SELECT \"ID\", \"Type\", \"Description\" FROM \"Victories\" ORDER BY \"ID\"", 1},
			{"SELECT \"ID\", \"Type\", \"Description\" FROM \"MinorCivilizations\" ORDER BY \"ID\"", 1},
			{"SELECT \"FileName\", \"OptionID\", \"Name\", \"Description\", \"ReadOnly\", \"Hidden\", \"DefaultValue\", \"SortPriority\" FROM \"MapScriptOptions\" ORDER BY \"FileName\", rowid", 2},
			{"SELECT \"FileName\", \"OptionID\", \"Name\", \"Description\", \"Value\", \"SortIndex\" FROM \"MapScriptOptionPossibleValues\" ORDER BY \"FileName\", \"OptionID\", \"SortIndex\"", 3},
		};
		const V3Table& def = kV3[table - GC_CATALOG_GAME_OPTIONS];
		GcqWriter xw(GC_Q_CATALOG, sizeof(gcq_catalog_ext));
		Database::Results rows;
		if (GC.GetGameDatabase()->Execute(rows, def.sql))
			while (rows.Step())
			{
				gcq_catalog_ext r;
				memset(&r, 0, sizeof(r));
				const char* t1 = rows.GetText(1);
				const char* t2 = rows.GetText(2);
				const char* t3 = def.mode == 1 ? NULL : rows.GetText(3);
				if (def.mode == 0)
				{
					r.n[0] = rows.GetInt(0);
					r.n[1] = rows.GetInt(4);
					r.n[2] = rows.GetInt(5);
					r.n[3] = rows.GetInt(6);
					r.s[0] = xw.Str(t1 ? t1 : "");
					r.s[1] = xw.Str(LocalizedName(t2).c_str());
					r.s[2] = xw.Str(LocalizedName(t3).c_str());
					r.s[3] = xw.Str("");
				}
				else if (def.mode == 1)
				{
					r.n[0] = rows.GetInt(0);
					r.s[0] = xw.Str(t1 ? t1 : "");
					r.s[1] = xw.Str(LocalizedName(t2).c_str());
					r.s[2] = xw.Str("");
					r.s[3] = xw.Str("");
				}
				else
				{
					// 2: MapScriptOptions (file, id, name, help, read only, hidden, default, sort); 3: possible values (file, id, name, help, value, sort index).
					const char* file = rows.GetText(0);
					r.s[0] = xw.Str(file ? file : "");
					r.s[1] = xw.Str(t1 ? t1 : "");
					r.s[2] = xw.Str(LocalizedName(t2).c_str());
					r.s[3] = xw.Str(LocalizedName(t3).c_str());
					if (def.mode == 2)
					{
						r.n[0] = rows.GetInt(7);
						r.n[1] = rows.GetInt(6);
						r.n[2] = rows.GetInt(4);
						r.n[3] = rows.GetInt(5);
					}
					else
					{
						r.n[0] = rows.GetInt(4);
						r.n[1] = rows.GetInt(5);
					}
				}
				xw.Add(&r);
			}
		return xw.Finish(out) ? GC_OK : Internal(msg, "out of memory");
	}
	switch (table)
	{
	case GC_CATALOG_UNITS: CollectInfos(GC.getUnitInfo(), entries); break;
	case GC_CATALOG_BUILDINGS: CollectInfos(GC.getBuildingInfo(), entries); break;
	case GC_CATALOG_PROJECTS: CollectInfos(GC.getProjectInfo(), entries); break;
	case GC_CATALOG_TECHNOLOGIES: CollectInfos(GC.getTechInfo(), entries); break;
	case GC_CATALOG_POLICIES: CollectInfos(GC.getPolicyInfo(), entries); break;
	case GC_CATALOG_TERRAINS: CollectInfos(GC.getTerrainInfo(), entries); break;
	case GC_CATALOG_FEATURES: CollectInfos(GC.getFeatureInfo(), entries); break;
	case GC_CATALOG_RESOURCES: CollectInfos(GC.getResourceInfo(), entries); break;
	case GC_CATALOG_IMPROVEMENTS: CollectInfos(GC.getImprovementInfo(), entries); break;
	case GC_CATALOG_ROUTES: CollectInfos(GC.getRouteInfo(), entries); break;
	case GC_CATALOG_CIVILIZATIONS: CollectInfos(GC.getCivilizationInfo(), entries); break;
	case GC_CATALOG_LEADERS: CollectInfos(GC.getLeaderHeadInfo(), entries); break;
	case GC_CATALOG_ERAS: CollectInfos(GC.getEraInfo(), entries); break;
	case GC_CATALOG_HANDICAPS: CollectInfos(GC.getHandicapInfo(), entries); break;
	case GC_CATALOG_PROMOTIONS: CollectInfos(GC.getPromotionInfo(), entries); break;
	default: return Invalid(msg, "Q_CATALOG: table out of range");
	}
	std::map<std::string, CatalogArt> art;
	LoadArt(kTables[table], art);

	GcqWriter w(GC_Q_CATALOG, sizeof(gcq_catalog));
	w.Reserve((uint32_t)entries.size());
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const CatalogEntry& e = entries[i];
		const std::map<std::string, CatalogArt>::const_iterator it = art.find(e.type);
		const CatalogArt none;
		const CatalogArt& a = it != art.end() ? it->second : none;
		gcq_catalog r;
		memset(&r, 0, sizeof(r));
		r.id = (int16_t)e.id;
		r.icon_index = (int16_t)(a.hasIcon ? a.iconIndex : -1);
		r.flag_index = (int16_t)(a.hasFlag ? a.flagIndex : -1);
		r.type = w.Str(e.type.c_str());
		r.name = w.Str(e.name.c_str());
		r.art_define = a.hasArt ? w.Str(a.artDefine.c_str()) : GCQ_NO_STRING;
		r.icon_atlas = a.hasIcon ? w.Str(a.iconAtlas.c_str()) : GCQ_NO_STRING;
		r.flag_atlas = a.hasFlag ? w.Str(a.flagAtlas.c_str()) : GCQ_NO_STRING;
		r.art_style = a.hasStyle ? w.Str(a.artStyle.c_str()) : GCQ_NO_STRING;
		r.strategic_view = GCQ_NO_STRING;
		w.Add(&r);
	}
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

// ---- Q_UNITS --------------------------------------------------------------------------------------------------

bool UnitBefore(const CvUnit* a, const CvUnit* b)
{
	if (a->getOwner() != b->getOwner()) return a->getOwner() < b->getOwner();
	return a->GetID() < b->GetID();
}

int32_t QueryUnits(int32_t viewer, gc_buf* out, std::string& msg)
{
	const bool all = viewer < 0;
	const TeamTypes team = all ? NO_TEAM : (TeamTypes)viewer;
	std::vector<CvUnit*> units;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CvPlayer& pl = GET_PLAYER((PlayerTypes)i);
		if (!pl.isEverAlive()) continue;
		const bool own = !all && (int)pl.getTeam() == viewer;
		int loop;
		for (CvUnit* u = pl.firstUnit(&loop); u; u = pl.nextUnit(&loop))
		{
			if (u->isDelayedDeath()) continue;
			if (!all && !own)
			{
				const CvPlot* plot = u->plot();
				if (!plot || !plot->isVisible(team, false) || u->isInvisible(team, false)) continue;
			}
			units.push_back(u);
		}
	}
	std::sort(units.begin(), units.end(), UnitBefore);

	GcqWriter w(GC_Q_UNITS, sizeof(gcq_unit));
	w.Reserve((uint32_t)units.size());
	for (size_t k = 0; k < units.size(); ++k)
	{
		const CvUnit* u = units[k];
		gcq_unit r;
		memset(&r, 0, sizeof(r));
		r.id = u->GetID();
		r.owner = (int8_t)u->getOwner();
		r.unit_type = (int16_t)u->getUnitType();
		r.x = (int16_t)u->getX();
		r.y = (int16_t)u->getY();
		r.hp = (int16_t)u->GetCurrHitPoints();
		r.max_hp = (int16_t)u->GetMaxHitPoints();
		r.moves_left = (int16_t)u->movesLeft();
		r.max_moves = (int16_t)u->maxMoves();
		r.strength = (int16_t)u->GetBaseCombatStrength();
		r.ranged = (int16_t)u->GetBaseRangedCombatStrength();
		r.range = Clamp8(u->GetRange());
		r.domain = (int8_t)u->getDomainType();
		r.activity = (int8_t)u->GetActivityType();
		r.level = Clamp8(u->getLevel());
		r.experience = (int16_t)u->getExperience();
		// The fortify mission puts the unit to sleep until the fortify turns count up, so that counts as fortified too.
		const bool fortified = u->getFortifyTurns() > 0 || u->IsFortifiedThisTurn() || (u->GetActivityType() == ACTIVITY_SLEEP && u->isFortifyable(true));
		unsigned flags = 0;
		if (u->canMove()) flags |= GC_UNIT_F_CAN_MOVE;
		if (u->ReadyToMove()) flags |= GC_UNIT_F_NEEDS_ORDERS;
		if (fortified) flags |= GC_UNIT_F_FORTIFIED;
		if (u->GetActivityType() == ACTIVITY_SLEEP && !fortified) flags |= GC_UNIT_F_SLEEPING;
		if (u->IsAutomated()) flags |= GC_UNIT_F_AUTOMATED;
		if (u->isEmbarked()) flags |= GC_UNIT_F_EMBARKED;
		if (!u->IsCombatUnit()) flags |= GC_UNIT_F_CIVILIAN;
		if (u->isPromotionReady()) flags |= GC_UNIT_F_CAN_PROMOTE;
		if (u->IsGarrisoned()) flags |= GC_UNIT_F_GARRISONED;
		if (u->isCargo()) flags |= GC_UNIT_F_CARGO;
		r.flags = (uint16_t)flags;
		// P13: the transport that carries the unit, what the unit builds, the way it faces.
		const CvUnit* transport = u->getTransportUnit();
		r.transport_id = transport ? transport->GetID() : -1;
		r.build_type = (int16_t)u->getBuildType();
		r.facing = (int8_t)u->getFacingDirection(false);
		const CvString name = u->getName();
		r.name = w.Str(name.c_str());
		w.Add(&r);
	}
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

// The unit of (player, id) of a Q_UNIT_ACTIONS / Q_PATH / Q_REACHABLE call, or NULL with msg.
CvUnit* FindQueryUnit(const char* query, int32_t player, int32_t id, std::string& msg)
{
	if (player < 0 || player >= MAX_PLAYERS)
	{
		msg = std::string(query) + ": player out of range";
		return NULL;
	}
	CvUnit* u = GET_PLAYER((PlayerTypes)player).getUnit(id);
	if (!u)
	{
		msg = std::string(query) + ": the player has no such unit";
		return NULL;
	}
	return u;
}

int32_t QueryUnitActions(int32_t player, int32_t unit, gc_buf* out, std::string& msg)
{
	CvUnit* u = FindQueryUnit("Q_UNIT_ACTIONS", player, unit, msg);
	if (!u) return GC_ERR_INVALID_ARG;
	GcqWriter w(GC_Q_UNIT_ACTIONS, sizeof(gcq_unit_action));
	for (int kind = GC_CMD_UNIT_MOVE_TO; kind <= GC_CMD_UNIT_WAKE; ++kind)
	{
		if (!UnitCommandApplies(u, kind)) continue;
		std::string why;
		gcq_unit_action r;
		memset(&r, 0, sizeof(r));
		r.action = (int16_t)kind;
		r.enabled = CheckUnitCommand(u, kind, false, -1, -1, why) ? 1 : 0;
		r.reason = r.enabled ? GCQ_NO_STRING : w.Str(why.c_str());
		w.Add(&r);
	}
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

// flags: GC_REACH_F_* of the step onto this node (v2); the first node of a path is the unit's own plot and has none.
void PathRecord(const PathStep& s, unsigned flags, gcq_path_node* r)
{
	memset(r, 0, sizeof(*r));
	r->x = (int16_t)s.x;
	r->y = (int16_t)s.y;
	r->turn = (int16_t)s.turn;
	r->moves_left = (int16_t)s.moves;
	r->flags = (uint16_t)flags;
}

int32_t QueryPath(const int32_t* args, gc_buf* out, std::string& msg)
{
	CvUnit* u = FindQueryUnit("Q_PATH", args[0], args[1], msg);
	if (!u) return GC_ERR_INVALID_ARG;
	CvMap& map = GC.getMap();
	if (args[2] < 0 || args[3] < 0 || args[2] >= map.getGridWidth() || args[3] >= map.getGridHeight()) return Invalid(msg, "Q_PATH: target is outside the map");
	GcqWriter w(GC_Q_PATH, sizeof(gcq_path_node));
	std::vector<PathStep> steps;
	if (args[2] == u->getX() && args[3] == u->getY())
	{
		// The target is the unit's own plot: nothing to walk, no records.
	}
	else if (FindUnitPath(u, args[2], args[3], steps))
	{
		for (size_t i = 0; i < steps.size(); ++i)
		{
			gcq_path_node r;
			const unsigned flags = i == 0 ? 0u : PathStepFlags(u, map.plot(steps[i - 1].x, steps[i - 1].y), map.plot(steps[i].x, steps[i].y), steps[i].moves, i + 1 == steps.size());
			PathRecord(steps[i], flags, &r);
			w.Add(&r);
		}
	}
	else
	{
		// v2: a target without a path is one node, the target, flagged UNREACHABLE (v1: an empty reply).
		gcq_path_node r;
		PathStep none;
		none.x = args[2];
		none.y = args[3];
		none.turn = 0;
		none.moves = 0;
		PathRecord(none, GC_REACH_F_UNREACHABLE, &r);
		w.Add(&r);
	}
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

// Every revealed plot within the unit's move budget (x3 for roads, at most 10 away) is tried with the interface pathfinder.
int32_t QueryReachable(const int32_t* args, gc_buf* out, std::string& msg)
{
	CvUnit* u = FindQueryUnit("Q_REACHABLE", args[0], args[1], msg);
	if (!u) return GC_ERR_INVALID_ARG;
	CvMap& map = GC.getMap();
	const TeamTypes team = GET_PLAYER(u->getOwner()).getTeam();
	GcqWriter w(GC_Q_REACHABLE, sizeof(gcq_path_node));
	int radius = (u->movesLeft() + GC.getMOVE_DENOMINATOR() - 1) / GC.getMOVE_DENOMINATOR() * 3;
	if (radius > 10) radius = 10;
	if (u->canMove() && !u->IsBusy() && radius > 0)
	{
		std::vector<PathStep> steps;
		const int x0 = u->getX(), y0 = u->getY();
		const int yLast = std::min((int)map.getGridHeight() - 1, y0 + radius);
		for (int y = std::max(0, y0 - radius); y <= yLast; ++y)
		{
			for (int x = 0; x < map.getGridWidth(); ++x)
			{
				if ((x == x0 && y == y0) || plotDistance(x0, y0, x, y) > radius) continue;
				const CvPlot* plot = map.plot(x, y);
				if (!plot || !plot->isRevealed(team, false)) continue;
				if (FindUnitPath(u, x, y, steps) && steps.back().turn == 0)
				{
					gcq_path_node r;
					const PathStep& last = steps.back();
					PathRecord(last, steps.size() < 2 ? 0u : PathStepFlags(u, map.plot(steps[steps.size() - 2].x, steps[steps.size() - 2].y), plot, last.moves, true), &r);
					w.Add(&r);
				}
			}
		}
	}
	return w.Finish(out) ? GC_OK : Internal(msg, "out of memory");
}

}  // namespace

int32_t RunGameQuery(int32_t kind, const int32_t* args, uint32_t argc, int32_t lastPumpStatus, int32_t blocking, const RtSession::BlockTarget& target, gc_buf* out, std::string& msg,
	const GameQueryExtra* extra)
{
	if (kind == GC_Q_CITIES || kind == GC_Q_TOP_BAR || kind == GC_Q_CITY_BUILDS || kind == GC_Q_CITY_ART || kind == GC_Q_TRADE_ROUTES || kind == GC_Q_TRADE_PATHS) return RunCityQuery(kind, args, argc, out, msg);  // G1a: gc_query_city.cpp
	switch (kind)
	{
	case GC_Q_GAME:
		if (argc > 1) return Invalid(msg, "Q_GAME takes no arguments or one: player");
		return QueryGame(args, argc, lastPumpStatus, blocking, target, extra, out, msg);
	case GC_Q_TURN:
		if (argc != 0) return Invalid(msg, "Q_TURN takes no arguments");
		return QueryTurn(extra, out, msg);
	case GC_Q_PLOTS:
	case GC_Q_PLAYERS:
	{
		const char* name = kind == GC_Q_PLOTS ? "Q_PLOTS" : "Q_PLAYERS";
		// v2: Q_PLOTS also takes plot indices after the viewer (only those plots).
		if (kind == GC_Q_PLOTS ? argc < 1 : argc != 1)
		{
			msg = std::string(name) + (kind == GC_Q_PLOTS ? " takes the argument viewer_team and optional plot indices" : " takes one argument: viewer_team");
			return GC_ERR_INVALID_ARG;
		}
		if (args[0] < -1 || args[0] >= MAX_TEAMS)
		{
			msg = std::string(name) + ": viewer_team out of range";
			return GC_ERR_INVALID_ARG;
		}
		return kind == GC_Q_PLOTS ? (argc > 1 ? QueryPlots(args[0], args + 1, argc - 1, out, msg) : QueryPlots(args[0], NULL, 0, out, msg)) : QueryPlayers(args[0], out, msg);
	}
	case GC_Q_NOTIFICATIONS: return QueryNotifications(args, argc, out, msg);
	case GC_Q_DIPLO: return QueryDiplo(args, argc, out, msg);  // N3.10: gc_diplo.cpp
	case GC_Q_CATALOG:
		if (argc != 1) return Invalid(msg, "Q_CATALOG takes one argument: table");
		if (args[0] < 0 || args[0] >= GC_CATALOG_TABLE_COUNT_V4) return Invalid(msg, "Q_CATALOG: table out of range");
		return QueryCatalog(args[0], out, msg);
	case GC_Q_UNITS:
		if (argc != 1) return Invalid(msg, "Q_UNITS takes one argument: viewer_team");
		if (args[0] < -1 || args[0] >= MAX_TEAMS) return Invalid(msg, "Q_UNITS: viewer_team out of range");
		return QueryUnits(args[0], out, msg);
	case GC_Q_UNIT_ACTIONS:
		if (argc != 2) return Invalid(msg, "Q_UNIT_ACTIONS takes two arguments: player, unit");
		return QueryUnitActions(args[0], args[1], out, msg);
	case GC_Q_PATH:
		if (argc != 4) return Invalid(msg, "Q_PATH takes four arguments: player, unit, x, y");
		return QueryPath(args, out, msg);
	case GC_Q_REACHABLE:
		if (argc != 2) return Invalid(msg, "Q_REACHABLE takes two arguments: player, unit");
		return QueryReachable(args, out, msg);
	case GC_Q_RANGE_TARGETS: return QueryRangeTargets(args, argc, out, msg);  // G1b: gc_query_range.cpp
	case GC_Q_REACHABLE_TREE: return QueryReachableTree(args, argc, out, msg);  // N3.9: gc_query_range.cpp
	case GC_Q_COMBAT_PREVIEW: return QueryCombatPreview(args, argc, out, msg);
	case GC_Q_PLOT_BASIS: return QueryPlotBasis(args, argc, out, msg);  // N3.8: gc_query_dirty.cpp
	case GC_Q_PLOTS_DIRTY:
		if (argc != 0) return Invalid(msg, "Q_PLOTS_DIRTY takes no arguments");
		if (!extra || !extra->dirty) return Internal(msg, "Q_PLOTS_DIRTY needs the ABI layer's snapshot");
		return extra->dirty->Query(out, msg);
	}
	return Invalid(msg, "unknown query kind");
}

}  // namespace lekmod_portable
