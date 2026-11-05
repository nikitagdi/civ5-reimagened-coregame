// Ported from headless/host/src/dllhost/HstDllHost.cpp and headless/host/src/control/HstSession.cpp (boot part of Run)
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <memory>
#include <string>
#include <vector>

#include "CvDllContext.h"
#include "CvLocalization.h"
#include "FireWorks/FMemoryStream.h"
#include "ICvDLLDatabaseUtility.h"
#include "CvDiplomacyRequests.h"
#include "rt_active.h"
#include "rt_fs.h"
#include "rt_lua.h"
#include "rt_save.h"
#include "rt_session.h"
#include "lekmod_portable/host_log.h"
#include "lekmod_portable/wb_map.h"

namespace lekmod_portable {

namespace {

bool g_session_exists = false;

// P14: CvWorldBuilderMap::Load reads a .Civ5Map through the host (gc_host.read_file); the loaded session's host is kept here.
gc_host g_wb_host;

bool ReadWbMap(void* user, const char* path, std::string& bytes)
{
	const gc_host* host = (const gc_host*)user;
	gc_buf buf = {0, 0, 0, 0};
	if (!host || !host->read_file || host->read_file(host->user, path, &buf) != GC_OK) return false;
	bytes.assign((const char*)buf.data, buf.len);
	gc_buf_release(&buf);
	return true;
}

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

// HstDatabase::ParseGuid: 32 hex digits, separators ignored.
bool ParseGuid(const char* hex, GUID& out)
{
	unsigned char nib[32];
	int n = 0;
	for (const char* p = hex; *p && n < 32; ++p)
	{
		char c = *p;
		if (c >= '0' && c <= '9') nib[n++] = (unsigned char)(c - '0');
		else if (c >= 'a' && c <= 'f') nib[n++] = (unsigned char)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') nib[n++] = (unsigned char)(c - 'A' + 10);
	}
	if (n != 32) return false;
	unsigned int b[16];
	for (int i = 0; i < 16; ++i) b[i] = (nib[2 * i] << 4) | nib[2 * i + 1];
	out.Data1 = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
	out.Data2 = (unsigned short)((b[4] << 8) | b[5]);
	out.Data3 = (unsigned short)((b[6] << 8) | b[7]);
	for (int i = 0; i < 8; ++i) out.Data4[i] = (unsigned char)b[8 + i];
	return true;
}

// HstDatabase::GetActiveDlc.
void GetActiveDlc(Database::Connection* db, std::vector<GUID>& out)
{
	Database::Results r;
	if (!db || !db->Execute(r, "SELECT PackageID FROM DownloadableContent WHERE IsActive = 1 ORDER BY rowid")) return;
	while (r.Step())
	{
		GUID g;
		const char* sz = r.GetText(0);
		if (sz && ParseGuid(sz, g)) out.push_back(g);
	}
}

}  // namespace

const char* RtSession::StepName(int step)
{
	static const char* const names[] = {"none", "D1", "D2", "B1", "B2", "B3", "B4", "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8", "done"};
	return (step >= 0 && step <= STEP_DONE) ? names[step] : "?";
}

RtSession::RtSession() : m_ctx(NULL), m_scripts(NULL), m_loaded(false), m_attached(false), m_inGame(false), m_newGameTried(false), m_turnSlices(0), m_noHumanInput(false), m_noHumanInputSet(false), m_humanPhaseTurn(-1), m_dataHash(0) {}

RtSession::~RtSession() { Unload(); }

bool RtSession::ReadGameFile(const gc_host& host, const char* path, std::vector<unsigned char>& out)
{
	gc_buf buf = {0, 0, 0, 0};
	if (!host.read_file || host.read_file(host.user, path, &buf) != GC_OK) return false;
	out.assign((const unsigned char*)buf.data, (const unsigned char*)buf.data + buf.len);
	gc_buf_release(&buf);
	return !out.empty();
}

bool RtSession::Load(const gc_host& host, LoadStatus& st)
{
#define RT_STEP(s) (st.step = (s))
#define RT_FAIL(...) do { st.error = Format(__VA_ARGS__); Log(LOG_ERROR, "session: %s: %s", StepName(st.step), st.error.c_str()); return false; } while (0)
	st = LoadStatus();
	if (m_loaded || g_session_exists) RT_FAIL("a game instance is already loaded in this process (one per process)");
	g_session_exists = true;

	// ./Civ5SavedGameDatabase.db (CvGame::reset inside GC.init) is served from memory.
	rt_fs::Install();
	rt_fs::Reset();
	g_wb_host = host;
	SetWbMapReader(ReadWbMap, &g_wb_host);

	// D1. DllMain creates the context singleton (CvGameCoreDLL.cpp:42).
	RT_STEP(STEP_D1);
	LekmodPortable_DllAttach();
	m_attached = true;
	// D2.
	RT_STEP(STEP_D2);
	m_ctx = CvDllGameContext::GetSingleton();
	if (!m_ctx) RT_FAIL("CvDllGameContext::GetSingleton returned NULL");

	// B1-B4: the game DB and localization (civhost Run: OpenGameplay, AttachLocalization, InitLocalization).
	RT_STEP(STEP_B1);
	std::vector<unsigned char> gameplay, loc;
	if (!ReadGameFile(host, "db/gameplay.db", gameplay)) RT_FAIL("read_file(db/gameplay.db) failed");
	if (!ReadGameFile(host, "db/loc-en_us.db", loc)) RT_FAIL("read_file(db/loc-en_us.db) failed");
	RT_STEP(STEP_B2);
	m_dataHash = Xxh64(&gameplay[0], gameplay.size(), 0);  // G4a: part of BuildId (a save needs the same game data)
	if (!m_db.OpenGameplay(&gameplay[0], gameplay.size())) RT_FAIL("gameplay DB image (%u bytes) did not load", (unsigned)gameplay.size());
	std::vector<unsigned char>().swap(gameplay);  // sqlite holds its own copy
	Log(LOG_INFO, "db: gameplay loaded: %d tables, Civilizations=%d Units=%d", m_db.TableCount(), m_db.Count("Civilizations"), m_db.Count("Units"));
	RT_STEP(STEP_B3);
	if (!m_db.AttachLocalization(&loc[0], loc.size())) RT_FAIL("localization DB image (%u bytes) did not attach", (unsigned)loc.size());
	std::vector<unsigned char>().swap(loc);
	RT_STEP(STEP_B4);
	if (!m_db.InitLocalization("en_US")) RT_FAIL("Localization::Initialize(en_US) failed");
	Log(LOG_INFO, "db: localization ok: TXT_KEY_CIV_ROME_DESC = '%s'", Localization::Lookup("TXT_KEY_CIV_ROME_DESC").toUTF8());

	// I1. Cache folder: CvGame::reset (inside GC.init) recreates <cache>Civ5SavedGameDatabase.db; rt_fs serves it.
	RT_STEP(STEP_I1);
	m_engine.SetCacheFolderPath("./");

	// I2. The DLL QIs ICvEngineUtility4 and owns the result (deletes it on the next SetDLLIFace).
	RT_STEP(STEP_I2);
	if (!m_ctx->SetDLLIFace(&m_engine)) RT_FAIL("SetDLLIFace failed");

	// I3.
	RT_STEP(STEP_I3);
	m_ctx->SetGameDatabase(m_db.Connection());

	// I4. GC.init(): CvGame/CvMap, PerformDatabasePostProcessing, CacheGameDatabaseData.
	RT_STEP(STEP_I4);
	Log(LOG_INFO, "init: ctx->Init() ...");
	m_ctx->Init();
	Log(LOG_INFO, "init: ctx->Init() returned");

	// I5. Logging off (the exe reads these from config.ini right after Init: Net_ReadDebugConfig RVA 0x291ea0).
	RT_STEP(STEP_I5);
	m_ctx->SetLogging(false);
	m_ctx->SetRandLogging(0);
	m_ctx->SetAILogging(false);
	m_ctx->SetAIPerfLogging(false);
	m_ctx->SetBuilderAILogging(false);
	m_ctx->SetPlayerAndCityAILogSplit(false);
	m_ctx->SetTutorialLogging(false);
	m_ctx->SetTutorialDebugging(false);
	m_ctx->SetPostTurnAutosaves(false);

	// I6. After Init in the exe.
	RT_STEP(STEP_I6);
	m_ctx->SetEngineUserInterface(&m_ui);
	m_ui.SetDiplo(&m_diplo);
	m_ui.SetEvents(&m_events);
	m_ui.SetReplica(&m_uiRep);
	m_engine.SetUiReplica(&m_uiRep);
	m_engine.SetDiplo(&m_diplo);
	m_engine.SetEvents(&m_events);
	m_engine.SetContext(m_ctx);  // net handler + pregame for the loopback (rt_net)

	// I7. "Refresh Gameplay DLL Data" (redundant with I4; kept as the exe does it in SetActiveDLCandMods).
	RT_STEP(STEP_I7);
	{
		std::auto_ptr<ICvDLLDatabaseUtility1> loader(m_ctx->GetDatabaseLoadUtility());
		if (!loader.get()) RT_FAIL("GetDatabaseLoadUtility returned NULL");
		st.flush = loader->FlushGameDatabaseData();
		st.postProcess = loader->PerformDatabasePostProcessing();
		st.cache = loader->CacheGameDatabaseData();
		Log(LOG_INFO, "init: database utility flush=%d postprocess=%d cache=%d", st.flush, st.postProcess, st.cache);
		if (!st.cache) RT_FAIL("CacheGameDatabaseData failed (flush=%d postprocess=%d)", st.flush, st.postProcess);
	}

	// I8. DLC allowed set from DownloadableContent (exe SetActiveDLCandMods tail).
	RT_STEP(STEP_I8);
	{
		std::vector<GUID> dlc;
		GetActiveDlc(m_db.Connection(), dlc);
		std::auto_ptr<ICvPreGame1> pregame(m_ctx->GetPreGame());
		pregame->clearDLCAllowed();
		for (size_t i = 0; i < dlc.size(); ++i) pregame->setDLCAllowed(dlc[i], true);
		st.dlcAllowed = (int)dlc.size();
		Log(LOG_INFO, "init: %d DLC packages allowed", st.dlcAllowed);
	}

	RT_STEP(STEP_DONE);
	m_loaded = true;
	return true;
#undef RT_FAIL
#undef RT_STEP
}

void RtSession::Unload()
{
	if (m_scripts)
	{
		m_engine.SetScriptSystem(NULL);
		delete m_scripts;
		m_scripts = NULL;
	}
	m_inGame = false;
	if (m_ctx)
	{
		m_ctx->SetGameDatabase(NULL);
		m_ctx->SetEngineUserInterface(NULL);
		m_ctx->SetDLLIFace(NULL);
		m_ctx = NULL;
	}
	if (m_attached)
	{
		// The game objects GC.init created stay with the process (civhost never uninitializes either); the
		// next gc instance in the same process is refused.
		LekmodPortable_DllDetach();
		m_attached = false;
	}
	m_loaded = false;
}

// ---- new game (M2) ---------------------------------------------------------------------------------------------------

const char* RtSession::NewGameStepName(int step)
{
	static const char* const names[] = {"none", "S1", "G0", "G1-G9", "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9", "done"};
	return (step >= 0 && step <= NG_DONE) ? names[step] : "?";
}

uint64_t RtSession::Fnv1a64(const void* data, size_t len, uint64_t hash)
{
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < len; ++i)
	{
		hash ^= p[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

std::string RtSession::MapText()
{
	CvMap& map = GC.getMap();
	const int w = map.getGridWidth(), h = map.getGridHeight();
	std::string text = Format("%d %d\n", w, h);
	for (int y = 0; y < h; ++y)
	{
		text += Format("%d:", y);
		for (int x = 0; x < w; ++x)
		{
			const CvPlot* p = map.plot(x, y);
			if (!p) { text += "?;"; continue; }
			text += Format("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d;", (int)p->getPlotType(), (int)p->getTerrainType(), (int)p->getFeatureType(),
				(int)p->getResourceType(NO_TEAM), p->getNumResource(), p->isNEOfRiver() ? 1 : 0, p->isWOfRiver() ? 1 : 0, p->isNWOfRiver() ? 1 : 0,
				(int)p->getRiverEFlowDirection(), (int)p->getRiverSEFlowDirection(), (int)p->getRiverSWFlowDirection());
		}
		text += "\n";
	}
	return text;
}

uint64_t RtSession::MapDigest()
{
	const std::string text = MapText();
	return Fnv1a64(text.data(), text.size());
}

bool RtSession::InitScripts(const gc_host& host, std::string& error)
{
	if (m_scripts) return true;
	if (!m_loaded || !m_ctx) { error = "session is not loaded"; return false; }
	if (!m_vfs.IsLoaded() && !m_vfs.Load(&host, error)) { error = "rt_vfs: " + error; return false; }
	RtScriptSystem* ss = new RtScriptSystem();
	// The DLL's libraries (Map, Game, Players, Fractal, GameInfoTypes, ...) need ctx->Init() (done in I4).
	if (!ss->Init(&m_vfs, m_db.Connection(), m_ctx->GetScriptSystemUtility()))
	{
		delete ss;
		error = "rt_lua: master state init failed";
		return false;
	}
	m_scripts = ss;
	ss->SetEvents(&m_events);
	ss->SetUiReplica(&m_uiRep);
	m_engine.SetScriptSystem(ss);
	Log(LOG_INFO, "lua: master state ready (%u files in vfs-index.json, %u include roots, %u contexts)", (unsigned)m_vfs.Files().size(),
		(unsigned)m_vfs.IncludeRoots().size(), (unsigned)m_vfs.Contexts().size());
	return true;
}

namespace {
double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// "assets/dlc/lekmod_v34.15/lua/foo/Bar.lua" -> "Bar": the include stem ContextPtr:LoadNewContext names in InGame.lua.
std::string ContextName(const std::string& path)
{
	size_t slash = path.find_last_of("\\/");
	std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
	if (s.size() > 4 && (s.compare(s.size() - 4, 4, ".lua") == 0 || s.compare(s.size() - 4, 4, ".LUA") == 0)) s.erase(s.size() - 4);
	return s;
}
}  // namespace

bool RtSession::NewGame(const gc_host& host, const std::string& setupJson, NewGameStatus& st)
{
#define NG_STEP(s) (st.step = (s))
#define NG_FAIL(...) do { st.error = Format(__VA_ARGS__); Log(LOG_ERROR, "session: new game %s: %s", NewGameStepName(st.step), st.error.c_str()); return false; } while (0)
	st = NewGameStatus();
	if (!m_loaded) NG_FAIL("session is not loaded");
	if (m_newGameTried)
	{
		// A caller error, not a game error: reported to the caller only (the ABI maps it to an error code).
		st.error = "a game was already started in this instance (one new game or load per instance)";
		Log(LOG_WARN, "session: new game refused: %s", st.error.c_str());
		return false;
	}
	m_newGameTried = true;

	// S1. Lua boot (last part).
	NG_STEP(NG_S1);
	std::string err;
	if (!InitScripts(host, err)) NG_FAIL("%s", err.c_str());
	RtScriptSystem& ss = *m_scripts;
	ss.CloseSaveData();
	ss.ResetGameState();
	const int errorsBoot = ss.GetErrorCount();

	// G0, G1-G9.
	NG_STEP(NG_G0);
	if (!m_setup.Parse(setupJson, err)) NG_FAIL("%s", err.c_str());
	m_setupJson = setupJson;
	NG_STEP(NG_G1);
	if (!m_setup.Apply(*this, err)) NG_FAIL("%s", err.c_str());
	m_engine.SetNetworkGame(m_setup.Network());
	m_engine.InitSlotConnections();  // P8: every seated human is on line at the start
	m_ui.SetPerPlayerCanEndTurn(m_setup.Network());
	st.setup = m_setup.Describe();
	Log(LOG_INFO, "%s", st.setup.c_str());

	// L1-L6.
	double t0 = NowMs();
	const bool init2 = LaunchToMap(st);
	st.msLaunch = NowMs() - t0;
	st.init2 = init2;
	SummarizeMap(st);
	st.mapDigest = MapDigest();
	{
		std::auto_ptr<ICvMap1> map(m_ctx->GetMap());
		FMemoryStream stream;
		map->Write(stream);
		const unsigned char* bytes = (const unsigned char*)stream.GetBuf();
		st.mapWrite.assign(bytes, bytes + stream.GetEOF());
		st.mapWriteDigest = Fnv1a64(bytes, stream.GetEOF());
	}
	st.luaErrorsMap = ss.GetErrorCount() - errorsBoot;
	Log(LOG_INFO, "map digest: %016llx (CvMap::Write digest %016llx); %d Lua error(s) during map generation, Init2 %.0f ms",
		(unsigned long long)st.mapDigest, (unsigned long long)st.mapWriteDigest, st.luaErrorsMap, st.msLaunch);
	if (!init2) NG_FAIL("game->Init2() returned false");

	// L7: gameplay contexts, in InGame.lua order (vfs-index.json "contexts"). The exe loads InGame during LoadGameState, before
	// NetPlayerReady (civhost GUESS timing).
	NG_STEP(NG_L7);
	const int errorsBeforeContexts = ss.GetErrorCount();
	t0 = NowMs();
	const std::vector<std::string>& contexts = m_vfs.Contexts();
	for (size_t i = 0; i < contexts.size(); ++i) ss.LoadContext(ContextName(contexts[i]).c_str());
	st.msContexts = NowMs() - t0;
	st.contextsTotal = (int)contexts.size();
	st.luaErrorsContexts = ss.GetErrorCount() - errorsBeforeContexts;
	st.contextsLoaded = ss.LoadedContextCount();
	Log(LOG_INFO, "lua: %d/%d gameplay context(s) loaded (%.0f ms, %d Lua error(s))", st.contextsLoaded, st.contextsTotal, st.msContexts, st.luaErrorsContexts);

	// L8, L9.
	const int errorsBeforeStart = ss.GetErrorCount();
	t0 = NowMs();
	StartGame(st);
	st.msStart = NowMs() - t0;
	st.luaErrorsStart = ss.GetErrorCount() - errorsBeforeStart;
	st.mapDigestStarted = MapDigest();
	{
		std::auto_ptr<ICvGame1> game(m_ctx->GetGame());
		st.turn = game->GetGameTurn();
	}
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isAlive()) continue;
		if (p.isMinorCiv()) ++st.minorsAlive;
		else if (!p.isBarbarian()) ++st.majorsAlive;
		st.units += p.getNumUnits();
	}
	Log(LOG_INFO, "game started: turn %d, %d major / %d minor civ(s) alive, %d unit(s), %d SequenceGameInitComplete listener(s), %d Lua error(s)",
		st.turn, st.majorsAlive, st.minorsAlive, st.units, st.initListeners, st.luaErrorsStart);
	NG_STEP(NG_DONE);
	m_inGame = true;
	return true;
#undef NG_FAIL
#undef NG_STEP
}

// Ported from HstGameRunner::LaunchToMap: exe SPLaunch 0x18cbc0 -> LaunchGame 0x370820 (GameInit 0x75c40, Init2 0x370aef).
bool RtSession::LaunchToMap(NewGameStatus& st)
{
	// L1 (no-op when already cached)
	st.step = NG_L1;
	{
		std::auto_ptr<ICvDLLDatabaseUtility1> loader(m_ctx->GetDatabaseLoadUtility());
		loader->CacheGameDatabaseData();
	}
	std::auto_ptr<ICvPreGame1> pre(m_ctx->GetPreGame());
	// L2: sets s_gameStarted; BeginSendBundle / sendPlayerInfo / EndSendBundle are sinks (CvPreGame.cpp:817).
	st.step = NG_L2;
	pre->closeInactiveSlots();

	// L3 (exe GameInit 0x75c40): the game handicap is the integer average of the handicaps of slots 0..62 whose status is SS_TAKEN or
	// SS_OPEN; STANDARD_HANDICAP when there is none.
	st.step = NG_L3;
	std::auto_ptr<ICvGame1> game(m_ctx->GetGame());
	int sum = 0, slots = 0;
	for (int i = 0; i < MAX_PLAYERS - 1; ++i)
	{
		const SlotStatus status = pre->slotStatus((PlayerTypes)i);
		if (status == SS_TAKEN || status == SS_OPEN)
		{
			sum += (int)pre->handicap((PlayerTypes)i);
			++slots;
		}
	}
	const HandicapTypes handicap = slots > 0 ? (HandicapTypes)(sum / slots) : (HandicapTypes)m_ctx->GetSTANDARD_HANDICAP();
	Log(LOG_INFO, "launch: game->Init(handicap %d) (%d taken/open slot(s))", (int)handicap, slots);
	double tInit = NowMs();
	game->Init(handicap);
	m_setup.AfterGameInit();   // P14: a WorldBuilder scenario's start year (CvGame::init resets it)
	st.msInit = NowMs() - tInit;

	// L4: InitPlayers, InitMap (Lua map script through gDLL->GetScriptSystem()), initDiplomacy, setInitialItems, ...
	st.step = NG_L4;
	Log(LOG_INFO, "launch: game->Init2() ...");
	tInit = NowMs();
	const bool init2 = game->Init2();
	st.msInit2 = NowMs() - tInit;
	Log(LOG_INFO, "launch: game->Init2() returned %d", (int)init2);

	// L5a: ICvPlayer1::LaterInit for all 64 players -> CvPlayer::gameStartInit (danger plots, InitPlots).
	st.step = NG_L5;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> player(game->GetPlayer((PlayerTypes)i));
		if (player.get()) player->LaterInit();
	}
	// L5b: InitTacticalAnalysisMap(map->NumPlots()), right after the LaterInit loop in the same exe function.
	std::auto_ptr<ICvMap1> map(m_ctx->GetMap());
	game->InitTacticalAnalysisMap(map->NumPlots());
	// L6: virtual time (civhost GUESS: exe call site not found).
	st.step = NG_L6;
	game->SetInitialTime(0);
	return init2;
}

// Ported from HstGameRunner::StartGame (L8, exe NetPlayerReady_Execute 0x80c80) and HstSession::StartGame (L9).
void RtSession::StartGame(NewGameStatus& st)
{
	st.step = NG_L8;
	std::auto_ptr<ICvGame1> game(m_ctx->GetGame());
	Log(LOG_INFO, "launch: game->DoGameStarted() (turn %d)", game->GetGameTurn());
	game->DoGameStarted();
	game->SendPlayerOptions(true);
	ICvNetworkSyncronization1* sync = m_ctx->GetNetworkSyncronizer();
	if (sync)
	{
		sync->ClearCityDeltas();
		sync->ClearRandomDeltas();
		sync->ClearPlayerDeltas();
		sync->ClearPlotDeltas();
		sync->ClearUnitDeltas();
	}
	// L9: once per new game and once per load (civhost GUESS: after L8). Lekmod changes state here (dummy policies, ...).
	st.step = NG_L9;
	st.initListeners = m_scripts->FireEvent("SequenceGameInitComplete");
	Log(LOG_INFO, "lua: Events.SequenceGameInitComplete fired (%d listener(s))", st.initListeners);
}

// Ported from HstGameRunner::LogMapSummary.
void RtSession::SummarizeMap(NewGameStatus& st)
{
	CvMap& map = GC.getMap();
	st.width = map.getGridWidth();
	st.height = map.getGridHeight();
	st.plots = map.numPlots();
	st.wrapX = map.isWrapX();
	st.wrapY = map.isWrapY();
	int types[NUM_PLOT_TYPES] = {0};
	int resources = 0, starts = 0, land = 0;
	for (int i = 0; i < st.plots; ++i)
	{
		const CvPlot* p = map.plotByIndex(i);
		if (!p) continue;
		const int t = (int)p->getPlotType();
		if (t >= 0 && t < NUM_PLOT_TYPES) ++types[t];
		if (p->getResourceType(NO_TEAM) != NO_RESOURCE) ++resources;
		if (p->isStartingPlot()) ++starts;
		if (!p->isWater()) ++land;
	}
	st.startPlots = starts;
	Log(LOG_INFO, "map: %d x %d = %d plots, wrapX=%d wrapY=%d; mountain=%d hills=%d land=%d ocean=%d; %d land plots, %d with resources, %d start plots",
		st.width, st.height, st.plots, (int)st.wrapX, (int)st.wrapY, types[PLOT_MOUNTAIN], types[PLOT_HILLS], types[PLOT_LAND], types[PLOT_OCEAN], land,
		resources, starts);
}

// ---- turn loop (A9) ----------------------------------------------------------------------------------------------------

const char* RtSession::PumpStatusName(int status)
{
	static const char* const names[] = {"WAITING_HUMAN", "AI_RUNNING", "GAME_OVER", "STUCK"};
	return (status >= 0 && status <= PUMP_STUCK) ? names[status] : "?";
}

const char* RtSession::BlockingName(int blocking)
{
	static const char* const names[] = {"ENDTURN_BLOCKING_POLICY", "ENDTURN_BLOCKING_RESEARCH", "ENDTURN_BLOCKING_PRODUCTION", "ENDTURN_BLOCKING_UNITS",
		"ENDTURN_BLOCKING_DIPLO_VOTE", "ENDTURN_BLOCKING_MINOR_QUEST", "ENDTURN_BLOCKING_FREE_TECH", "ENDTURN_BLOCKING_STACKED_UNITS",
		"ENDTURN_BLOCKING_UNIT_NEEDS_ORDERS", "ENDTURN_BLOCKING_UNIT_PROMOTION", "ENDTURN_BLOCKING_CITY_RANGE_ATTACK", "ENDTURN_BLOCKING_FREE_POLICY",
		"ENDTURN_BLOCKING_FREE_ITEMS", "ENDTURN_BLOCKING_FOUND_PANTHEON", "ENDTURN_BLOCKING_FOUND_RELIGION", "ENDTURN_BLOCKING_ENHANCE_RELIGION",
		"ENDTURN_BLOCKING_STEAL_TECH", "ENDTURN_BLOCKING_MAYA_LONG_COUNT", "ENDTURN_BLOCKING_FAITH_GREAT_PERSON", "ENDTURN_BLOCKING_ADD_REFORMATION_BELIEF",
		"ENDTURN_BLOCKING_LEAGUE_CALL_FOR_PROPOSALS", "ENDTURN_BLOCKING_CHOOSE_ARCHAEOLOGY", "ENDTURN_BLOCKING_LEAGUE_CALL_FOR_VOTES",
		"ENDTURN_BLOCKING_CHOOSE_IDEOLOGY"};
	if (blocking == NO_ENDTURN_BLOCKING_TYPE) return "NONE";
	return (blocking >= 0 && blocking < (int)(sizeof(names) / sizeof(names[0])) && blocking < NUM_ENDTURN_BLOCKING_TYPES) ? names[blocking] : "?";
}

const char* RtSession::TargetKindName(int kind)
{
	static const char* const names[] = {"none", "unit", "city", "plot"};
	return (kind >= 0 && kind <= TARGET_PLOT) ? names[kind] : "?";
}

const char* RtSession::EndTurnStatusName(int status)
{
	static const char* const names[] = {"ACCEPTED", "REJECTED_BLOCKED", "REJECTED_NOT_READY"};
	return (status >= 0 && status <= END_TURN_REJECTED_NOT_READY) ? names[status] : "?";
}

// The human the no-argument calls and the pump's active_player / blocker speak of (audit F-07: the active player of a network game is the server seat, not a
// human): the first human the turn waits for in a network game, the active player in a single game.
PlayerTypes RtSession::LeadHuman()
{
	if (!m_inGame) return NO_PLAYER;
	if (!m_engine.NetworkGame()) return m_engine.GetActivePlayer();
	const std::vector<PlayerTypes> w = WaitingHumans();
	return w.empty() ? NO_PLAYER : w.front();
}

bool RtSession::IsHumanWaiting() { return IsHumanWaiting(LeadHuman()); }

bool RtSession::IsHumanWaiting(PlayerTypes who)
{
	if (!m_inGame || who == NO_PLAYER) return false;
	const CvPlayer& p = GET_PLAYER(who);
	return p.isHuman() && !p.isObserver() && p.isAlive() && p.isTurnActive() && m_engine.allAICivsProcessedThisTurn() && GC.getGame().allUnitAIProcessed() &&
		!m_engine.TurnCompleteSent(who) && m_engine.NetQueueSize() == 0 && !p.hasBusyUnitOrCity();
}

std::vector<PlayerTypes> RtSession::WaitingHumans()
{
	std::vector<PlayerTypes> out;
	for (int i = 0; i < MAX_PLAYERS; ++i)
		if (IsHumanWaiting((PlayerTypes)i)) out.push_back((PlayerTypes)i);
	return out;
}

bool RtSession::SeatedHumanBusy() const
{
	if (!m_inGame) return false;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if (IsSeatedHuman(i) && GET_PLAYER((PlayerTypes)i).hasBusyUnitOrCity()) return true;
	return false;
}

bool RtSession::HumanPhase() const
{
	return m_inGame && m_engine.allAICivsProcessedThisTurn() && GC.getGame().allUnitAIProcessed();
}

int RtSession::HumansLeft() const
{
	if (!HumanPhase()) return 0;
	int n = 0;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if (IsSeatedHuman(i) && !m_engine.TurnCompleteSent((PlayerTypes)i)) ++n;
	return n;
}

int RtSession::SeatedHumans() const
{
	int n = 0;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if (IsSeatedHuman(i)) ++n;
	return n;
}

bool RtSession::EverHuman(int player) const
{
	if (player < 0 || player >= MAX_MAJOR_CIVS) return false;
	if (m_setup.Network() && player < (int)m_setup.players.size() && m_setup.players[player].human) return true;
	return CvPreGame::slotStatus((PlayerTypes)player) == SS_TAKEN;
}

bool RtSession::AllSeatedHumansDead() const
{
	if (!m_inGame || !m_engine.NetworkGame()) return false;
	int taken = 0, alive = 0;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		if (CvPreGame::slotStatus((PlayerTypes)i) != SS_TAKEN) continue;
		++taken;
		if (GET_PLAYER((PlayerTypes)i).isAlive()) ++alive;
	}
	return taken > 0 && alive == 0;
}

// Spike S1: what the B1 slice does for the one human, done for each human whose turn is active, with that human
// as the active player: UpdateTestEndTurn (the gamecore computes the end-turn blocker only for the active player), the diplomacy requests the
// gamecore queued for P (CvDiplomacyRequests.cpp:223-240; they are sent only while P is active, :131-146) and their auto-decline (SIMP-003),
// the host canEndTurn, the auto-choices (SIMP-002). Then the exe's frame (the turn handshake). True when an auto-choice was made.
bool RtSession::NetworkFrame()
{
	std::auto_ptr<ICvGame1> game(m_ctx->GetGame());
	bool autoChose = false;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isAlive() || !p.isHuman() || p.isObserver() || !p.isTurnActive()) continue;
		ActiveAs as((PlayerTypes)i);
		// A human who took over an AI slot (gc_slot_control CONNECT, rt_slots.cpp) starts its first turn with the AI's automoves flag still set: CvPlayer::setEndTurn(false) does
		// nothing for a player that never had its end of turn set (CvPlayer.cpp:21258), and that is where the flag goes off. A human's turn starts with it off; with it on the
		// turn would not be a safe point (a human in auto moves).
		if (p.isAutoMoves() && !p.isEndTurn()) p.setAutoMoves(false);
		game->UpdateTestEndTurn();
		if (p.GetDiplomacyRequests()) p.GetDiplomacyRequests()->Update();
		const size_t diploSeen = m_diplo.Events().size();
		m_diplo.Process((PlayerTypes)i);
		PushDiploEvents(diploSeen);
		UpdateHostCanEndTurn();
		autoChose = m_autochoice.Run(*this, (PlayerTypes)i) || autoChose;
	}
	// Leader messages raised while the server seat was active (none expected: the gamecore queues them per human in a network game).
	if (m_diplo.Pending() > 0)
	{
		const size_t diploSeen = m_diplo.Events().size();
		m_diplo.Process();
		PushDiploEvents(diploSeen);
	}
	m_engine.ExeFrame();
	return autoChose;
}

int RtSession::CurrentBlocking(BlockTarget* target) { return CurrentBlocking(target, LeadHuman()); }

int RtSession::CurrentBlocking(BlockTarget* target, PlayerTypes active)
{
	if (target) *target = BlockTarget();
	if (!m_inGame || active == NO_PLAYER) return NO_ENDTURN_BLOCKING_TYPE;
	const CvPlayer& p = GET_PLAYER(active);
	const int blocking = (int)p.GetEndTurnBlockingType();
	if (!target || blocking == NO_ENDTURN_BLOCKING_TYPE) return blocking;
	BlockTarget& t = *target;
	if (blocking == ENDTURN_BLOCKING_UNITS || blocking == ENDTURN_BLOCKING_STACKED_UNITS || blocking == ENDTURN_BLOCKING_UNIT_PROMOTION)
	{
		// updateTestEndTurn derives these from the units, not from a notification (CvGame.cpp:2364-2383).
		const CvUnit* unit = NULL;
		if (blocking == ENDTURN_BLOCKING_UNIT_PROMOTION)
		{
			int loop;
			for (const CvUnit* u = p.firstUnit(&loop); u; u = p.nextUnit(&loop))
				if (u->isPromotionReady() && !u->isDelayedDeath()) { unit = u; break; }
		}
		else
			unit = p.GetFirstReadyUnit();
		if (unit)
		{
			t.kind = TARGET_UNIT;
			t.owner = (int)unit->getOwner();
			t.id = unit->GetID();
			t.x = unit->getX();
			t.y = unit->getY();
		}
		return blocking;
	}
	t.notification = p.GetEndTurnBlockingNotificationIndex();
	// The notification of the blocker, of this player (N3.7: a network human's list is read from the core, not from the active player's registry).
	RtNotification note;
	if (!m_ui.FindNotificationOf(active, t.notification, &note)) return blocking;
	const RtNotification* n = &note;
	t.notificationType = n->type;
	const CvPlot* plot = (n->x >= 0 && n->y >= 0) ? GC.getMap().plot(n->x, n->y) : NULL;
	if (!plot) return blocking;
	t.x = n->x;
	t.y = n->y;
	const CvCity* city = plot->getPlotCity();
	if (city && city->getOwner() == active)
	{
		t.kind = TARGET_CITY;
		t.owner = (int)active;
		t.id = city->GetID();
	}
	else
		t.kind = TARGET_PLOT;
	return blocking;
}

std::string RtSession::StuckText()
{
	CvGame& g = GC.getGame();
	std::string s = Format("game: turn-active players %d, waiting for blocking input %d, AI civs processed %d, unit AI processed %d; still active:",
		g.getNumGameTurnActive(), g.IsWaitingForBlockingInput() ? 1 : 0, m_engine.allAICivsProcessedThisTurn() ? 1 : 0, g.allUnitAIProcessed() ? 1 : 0);
	std::string diplo;
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isAlive() || !p.isTurnActive()) continue;
		s += Format(" p%d(%s end=%d diplo=%d busy=%d ready=%d tc=%d)", i, p.isHuman() ? "human" : "ai", p.isEndTurn() ? 1 : 0, p.HasActiveDiplomacyRequests() ? 1 : 0,
			p.hasBusyUnitOrCity() ? 1 : 0, p.hasReadyUnit() ? 1 : 0, m_engine.HasReceivedTurnComplete((PlayerTypes)i) ? 1 : 0);
		if (!p.isHuman() && p.HasActiveDiplomacyRequests()) diplo += Format(" %d", i);
	}
	// CheckPlayerTurnDeactivate keeps an AI turn-active while its leader message to the human is open (CvGame.cpp:1752); only
	// ActiveDiplomacyRequestComplete closes it (RtDiplo::Process, SIMP-003).
	if (!diplo.empty()) s += Format(" -- AI player(s)%s wait for the human's answer to a leader message (RtDiplo pending %d: the host answers after Update)", diplo.c_str(), m_diplo.Pending());
	if (m_engine.NetworkGame())
	{
		s += Format("; net: barrier turn %d, all-AI-processed turn %d, TurnAllComplete sent %d, can advance %d; humans:", m_engine.BarrierTurn(), m_engine.AllAIProcessedTurn(),
			m_engine.HasSentTurnAllComplete() ? 1 : 0, m_engine.CanAdvanceTurn() ? 1 : 0);
		for (int i = 0; i < MAX_PLAYERS; ++i)
		{
			const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
			if (!p.isAlive() || !p.isHuman() || p.isObserver()) continue;
			s += Format(" p%d(active=%d end=%d automoves=%d processed=%d sent=%d tc=%d tac=%d simult=%d)", i, p.isTurnActive() ? 1 : 0, p.isEndTurn() ? 1 : 0, p.isAutoMoves() ? 1 : 0,
				p.hasProcessedAutoMoves() ? 1 : 0, m_engine.TurnCompleteSent((PlayerTypes)i) ? 1 : 0, m_engine.HasReceivedTurnComplete((PlayerTypes)i) ? 1 : 0,
				m_engine.HasReceivedTurnAllComplete((PlayerTypes)i) ? 1 : 0, p.isSimultaneousTurns() ? 1 : 0);
		}
	}
	return s;
}

// canEndTurn is true while the human's active turn has no blocker and nothing is busy. This is CvGame::updateTestEndTurn's
// single-player rule (CvGame.cpp:2386-2397) without its "no unit moved this turn" term: that term exists for the exe's auto end turn
// (off here), and with it a turn in which a unit moved could never be ended. The DLL resets the flag at turn start (CvGame.cpp:8770).
void RtSession::UpdateHostCanEndTurn()
{
	const PlayerTypes active = m_engine.GetActivePlayer();
	if (active == NO_PLAYER) return;
	const CvPlayer& p = GET_PLAYER(active);
	if (!p.isHuman() || p.isObserver() || !p.isAlive() || !p.isTurnActive()) return;  // v2 (audit F-07): not the server seat
	m_ui.HostSetCanEndTurn(p.GetEndTurnBlockingType() == NO_ENDTURN_BLOCKING_TYPE && !p.hasBusyUnitOrCity() && !p.hasReadyUnit());
}

// Ported from HstGameRunner::RunTurn / RunSlice (single mode): the loopback queue goes to the DLL before every Update(); the exe's frame
// then runs UpdateTestEndTurn (civhost never called it, so the human's turn never ended there).
// GC_EVENT_DIPLO_AUTO_DECLINED for the RtDiplo events from index first on (those RtDiplo::Process just added; a scenario taking the events
// itself does not disturb the index because Process only appends and the caller measures the size right before it).
// args: ai_player, kind = DiploUIStateTypes of the message; text: the leader's localized text, text2: the one-line description (state, answer).
void RtSession::PushDiploEvents(size_t first)
{
	const std::vector<RtDiploEvent>& evs = m_diplo.Events();
	for (size_t i = first; i < evs.size(); ++i)
	{
		if (evs[i].answer == RTDIPLO_DISMISSED) continue;  // a statement without choices (war declaration, AI answer): nothing was declined
		const std::string line = RtDiplo::Describe(evs[i]);
		m_events.Push(GC_EVENT_DIPLO_AUTO_DECLINED, 3, evs[i].from, evs[i].state, evs[i].to, 0, 0, 0, evs[i].text.c_str(), line.c_str());  // v2: args end with the human who was offered
	}
}

RtSession::PumpResult RtSession::Pump(uint32_t maxSlices, bool stopOnTurnChange)
{
	PumpResult r;
	const double t0 = NowMs();
	if (!m_inGame || !m_ctx)
	{
		r.status = PUMP_STUCK;
		r.reason = "no game is running (NewGame has not completed)";
		return r;
	}
	std::auto_ptr<ICvGame1> game(m_ctx->GetGame());
	RtCoreScope core(m_engine);
	int turn = game->GetGameTurn();
	r.turn = turn;
	bool done = false;
	uint32_t settle = 0;  // slices run past WAITING_HUMAN for a busy unit or city of a human (network game)
	while (!done)
	{
		// the server seat never dies, so the core's "the local player died" rule (CvGame.cpp:1705) cannot end a network game.
		if (game->GetGameState() != GAMESTATE_OVER && AllSeatedHumansDead())
		{
			Log(LOG_INFO, "session: turn %d: every human player is dead: the game is over", game->GetGameTurn());
			GC.getGame().setGameState(GAMESTATE_OVER);
		}
		if (game->GetGameState() == GAMESTATE_OVER)
		{
			r.status = PUMP_GAME_OVER;
			r.reason = "the game is over";
			break;
		}
		if (r.slices >= maxSlices) break;  // AI_RUNNING
		r.netMessages += m_engine.DispatchNetMessages();
		m_engine.AdvanceStep();
		game->Update();
		bool autoChose = false;
		if (m_engine.NetworkGame())
			autoChose = NetworkFrame();
		else
		{
			game->UpdateTestEndTurn();
			// SIMPLIFIED(SIMP-003): AI proposals / messages to the human are declined here, after Update returned (not inside the DLL callback).
			const size_t diploSeen = m_diplo.Events().size();
			m_diplo.Process();
			PushDiploEvents(diploSeen);
			UpdateHostCanEndTurn();
			// F3: auto-choices for the human's blocker; a choice made means one more slice before WAITING_HUMAN.
			autoChose = m_autochoice.Run(*this);
		}
		// event 14 HUMAN_PHASE_STARTED: once per turn, when the AI civs are done and the humans may act. A server saves the start of the turn on it.
		if (m_engine.NetworkGame() && m_humanPhaseTurn != game->GetGameTurn() && HumanPhase())
		{
			m_humanPhaseTurn = game->GetGameTurn();
			m_events.Push(GC_EVENT_HUMAN_PHASE_STARTED, 1, m_humanPhaseTurn);
		}
		++r.slices;
		++m_turnSlices;
		const int now = game->GetGameTurn();
		if (now != turn)
		{
			turn = now;
			m_turnSlices = 0;
			++r.turnsAdvanced;
			m_events.Push(GC_EVENT_TURN_STARTED, 1, now);
			if (stopOnTurnChange) done = true;
		}
		else if (m_turnSlices >= kMaxSlicesPerTurn)
		{
			r.status = PUMP_STUCK;
			r.reason = Format("turn %d did not end within %u slices; %s", turn, (unsigned)kMaxSlicesPerTurn, StuckText().c_str());
			Log(LOG_WARN, "session: %s", r.reason.c_str());
			break;
		}
		if (!done && !autoChose && (m_engine.NetworkGame() ? !WaitingHumans().empty() : IsHumanWaiting()))
		{
			// Live game 931 (2026-10-30): one human being up is not the end of the pump while another human's unit or city is busy. A move
			// holds its unit busy for one slice per step (CvUnitMission::CalculateMissionTimer, counted down by CvPlayer::updateTimers in
			// Update), and with the pump ending at the first slice nothing counted it down any more: END_TURN of that human was refused
			// ("pump first") and gc_save had no save point. The exe's frame loop ran Update every frame; here the pump runs on until the
			// humans are settled (bounded: kMaxSettleSlices, and the caller's budget).
			if (m_engine.NetworkGame() && settle < kMaxSettleSlices && r.slices < maxSlices && SeatedHumanBusy())
			{
				++settle;
				continue;
			}
			// Human time does not count towards STUCK.
			m_turnSlices = 0;
			if (NoHumanInput() && !m_engine.NetworkGame())
			{
				// F5: nobody can answer a choice the auto-choices left to the human (flag off, or the choice could not clear it).
				const std::string why = m_autochoice.StuckReason(*this, (int)GET_PLAYER(m_engine.GetActivePlayer()).GetEndTurnBlockingType());
				if (!why.empty())
				{
					r.status = PUMP_STUCK;
					r.reason = Format("turn %d: %s", turn, why.c_str());
					Log(LOG_WARN, "session: %s", r.reason.c_str());
					break;
				}
			}
			r.status = PUMP_WAITING_HUMAN;
			break;
		}
	}
	r.turn = game->GetGameTurn();
	r.turnSlices = m_turnSlices;
	r.activePlayer = (int)(r.status == PUMP_WAITING_HUMAN || !m_engine.NetworkGame() ? LeadHuman() : NO_PLAYER);
	r.blocking = CurrentBlocking(&r.target);
	r.canEndTurn = m_ui.canEndTurn();
	r.humansLeft = HumansLeft();
	m_ui.SyncNetworkNotifications();  // N3.7: NOTIFICATION_ADDED / _REMOVED of every human, by comparison with the previous call
	if (m_engine.NetworkGame() && r.status == PUMP_WAITING_HUMAN) r.waitingHumans = (int)WaitingHumans().size();
	r.ms = NowMs() - t0;
	return r;
}

RtSession::EndTurnResult RtSession::EndTurn(bool force, PlayerTypes who)
{
	ActiveAs as(who);
	return EndTurn(force);
}

RtSession::EndTurnResult RtSession::EndTurn(bool force)
{
	EndTurnResult r;
	if (!m_inGame || !m_ctx)
	{
		r.reason = "no game is running";
		return r;
	}
	// v2 (audit F-07): without a human named, a network game ends the turn of the first human the turn waits for (the active player is the server seat).
	if (m_engine.NetworkGame() && !IsSeatedHuman((int)m_engine.GetActivePlayer()))
	{
		const PlayerTypes lead = LeadHuman();
		if (lead != NO_PLAYER) return EndTurn(force, lead);
	}
	std::auto_ptr<ICvGame1> game(m_ctx->GetGame());
	r.turn = game->GetGameTurn();
	// The human whose turn this ends is the active player here (the one named by EndTurn(force, who), under ActiveAs; the only human of a single game).
	const PlayerTypes me = m_engine.GetActivePlayer();
	if (!IsHumanWaiting(me))
	{
		const PlayerTypes active = m_engine.GetActivePlayer();
		if (active == NO_PLAYER || !GET_PLAYER(active).isHuman() || !GET_PLAYER(active).isAlive()) r.reason = "the active player is not a living human";
		else if (m_engine.HasSentTurnComplete()) r.reason = "turn complete was already sent this turn";
		else if (!GET_PLAYER(active).isTurnActive() || !m_engine.allAICivsProcessedThisTurn() || !GC.getGame().allUnitAIProcessed()) r.reason = "the AI turn is still running";
		else r.reason = "a command is queued or a unit / city is busy; pump first";
		r.blocking = CurrentBlocking(&r.target, me);
		return r;
	}
	// The blocker as of now (a command delivered since the last Pump may have changed it).
	game->UpdateTestEndTurn();
	UpdateHostCanEndTurn();
	r.blocking = CurrentBlocking(&r.target, me);
	CvGame& g = GC.getGame();
	if (r.blocking == NO_ENDTURN_BLOCKING_TYPE)
	{
		if (!m_ui.canEndTurn())
		{
			r.reason = "canEndTurn is false (a unit or city is busy)";
			return r;
		}
		g.doControl(CONTROL_ENDTURN);
	}
	else if (force && (r.blocking == ENDTURN_BLOCKING_UNITS || r.blocking == ENDTURN_BLOCKING_STACKED_UNITS))
	{
		r.forced = true;
		g.doControl(CONTROL_FORCEENDTURN);
	}
	else
	{
		r.status = END_TURN_REJECTED_BLOCKED;
		r.reason = Format("blocked by %s%s", BlockingName(r.blocking),
			(r.blocking == ENDTURN_BLOCKING_UNITS || r.blocking == ENDTURN_BLOCKING_STACKED_UNITS) ? " (FORCE_END_TURN would end it)" : "");
		return r;
	}
	if (m_engine.HasSentTurnComplete())
	{
		r.status = END_TURN_ACCEPTED;
		Log(LOG_INFO, "session: turn %d: the human ended the turn (%s)", r.turn, r.forced ? "CONTROL_FORCEENDTURN" : "CONTROL_ENDTURN");
		return r;
	}
	r.status = r.blocking == NO_ENDTURN_BLOCKING_TYPE ? END_TURN_REJECTED_NOT_READY : END_TURN_REJECTED_BLOCKED;
	r.reason = Format("the gamecore did not accept %s (blocker %s)", r.forced ? "CONTROL_FORCEENDTURN" : "CONTROL_ENDTURN", BlockingName(r.blocking));
	return r;
}

std::string RtSession::StateText()
{
	CvGame& game = GC.getGame();
	std::string text = Format("turn=%d rng=%u\n", game.getGameTurn(), (unsigned)game.getJonRand().getCallCount());
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isAlive()) continue;
		text += Format("p%d: cities=%d units=%d score=%d gold=%d techs=%d policies=%d pop=%d\n", i, p.getNumCities(), p.getNumUnits(), p.GetScore(),
			p.GetTreasury()->GetGold(), GET_TEAM(p.getTeam()).GetTeamTechs()->GetNumTechsKnown(), p.GetNumPolicies(), p.getTotalPopulation());
	}
	CvMap& map = GC.getMap();
	text += "owners:";
	for (int i = 0, n = map.numPlots(); i < n; ++i)
	{
		const CvPlot* p = map.plotByIndex(i);
		text += Format("%d,", p ? (int)p->getOwner() : -2);
	}
	text += "\n";
	return text;
}

uint64_t RtSession::StateDigest()
{
	const std::string text = StateText();
	const uint64_t h = Fnv1a64(text.data(), text.size());
	const std::string map = MapText();
	return Fnv1a64(map.data(), map.size(), h);
}

}  // namespace lekmod_portable
