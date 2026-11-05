// Scenario `load` (milestone M1, run.sh --scenario load): rt_session boot D1-D2, B1-B4, I1-I8 on the real game DB,
// then the key-table counts the gamecore cached, compared with civhost.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "CvDllContext.h"
#include "rt_fs.h"
#include "rt_selftest.h"
#include "rt_session.h"
#include "lekmod_portable/host_log.h"

using namespace lekmod_portable;

namespace {

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

// JSON string body: quotes and backslashes escaped, control characters dropped.
std::string Escape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		const char ch = s[i];
		if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
		else if ((unsigned char)ch >= 0x20) out += ch;
	}
	return out;
}

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}

	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += Format("%s{\"check\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}", json.empty() ? "" : ",", name, ok ? "true" : "false", Escape(detail).c_str());
		printf("load: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

int ScalarInt(Database::Connection& db, const char* sql)
{
	Database::Results r;
	if (!db.Execute(r, sql) || !r.Step()) return -1;
	return r.GetInt(0);
}

void ForwardLog(void* user, int level, const char* msg)
{
	const gc_host* host = static_cast<const gc_host*>(user);
	if (host && host->log) host->log(host->user, level, msg);
}

// civhost reference, same gameplay DB (Civ5DebugDatabase.db of the oracle cache, identical md5 to the game-data db/gameplay.db), from its boot log (civhost.log):
//   db: gameplay loaded: Civilizations=115 Units=230
//   dll: GetNumCivilizationInfos=116 GetNumUnitInfos=265 GetNumMinorCivInfos=59 GetNumLeaderHeadInfos=115
//   init: database utility flush=1 postprocess=1 cache=1;  init: 14 DLC packages allowed
// -1: civhost does not log the table; its invariant applies to all (HstSession.cpp after I1-I8: info arrays are
// indexed by ID, so the cached count is MAX(ID)+1).
struct KeyTable
{
	const char* table;
	int (*infos)();
	int civhostRows;
	int civhostInfos;
};

#define RT_INFOS(fn) int fn##_() { return (int)GC.fn(); }
RT_INFOS(getNumCivilizationInfos)
RT_INFOS(getNumMinorCivInfos)
RT_INFOS(getNumLeaderHeadInfos)
RT_INFOS(getNumUnitInfos)
RT_INFOS(getNumUnitClassInfos)
RT_INFOS(getNumUnitCombatClassInfos)
RT_INFOS(getNumBuildingInfos)
RT_INFOS(getNumBuildingClassInfos)
RT_INFOS(getNumTechInfos)
RT_INFOS(getNumEraInfos)
RT_INFOS(getNumPolicyInfos)
RT_INFOS(getNumPolicyBranchInfos)
RT_INFOS(getNumPromotionInfos)
RT_INFOS(getNumImprovementInfos)
RT_INFOS(getNumBuildInfos)
RT_INFOS(getNumRouteInfos)
RT_INFOS(getNumResourceInfos)
RT_INFOS(getNumResourceClassInfos)
RT_INFOS(getNumFeatureInfos)
RT_INFOS(getNumTerrainInfos)
RT_INFOS(getNumWorldInfos)
RT_INFOS(getNumClimateInfos)
RT_INFOS(getNumSeaLevelInfos)
RT_INFOS(getNumSpecialistInfos)
RT_INFOS(getNumHandicapInfos)
RT_INFOS(getNumGameSpeedInfos)
RT_INFOS(getNumVictoryInfos)
RT_INFOS(getNumBeliefInfos)
RT_INFOS(getNumReligionInfos)
RT_INFOS(getNumTraitInfos)
RT_INFOS(getNumProjectInfos)
RT_INFOS(getNumProcessInfos)
RT_INFOS(getNumGameOptionInfos)
RT_INFOS(getNumResolutionInfos)
RT_INFOS(getNumLeagueProjectInfos)
#undef RT_INFOS

const KeyTable kKeyTables[] = {
	{"Civilizations", getNumCivilizationInfos_, 115, 116},
	{"MinorCivilizations", getNumMinorCivInfos_, -1, 59},
	{"Leaders", getNumLeaderHeadInfos_, -1, 115},
	{"Units", getNumUnitInfos_, 230, 265},
	{"UnitClasses", getNumUnitClassInfos_, -1, -1},
	{"UnitCombatInfos", getNumUnitCombatClassInfos_, -1, -1},
	{"Buildings", getNumBuildingInfos_, -1, -1},
	{"BuildingClasses", getNumBuildingClassInfos_, -1, -1},
	{"Technologies", getNumTechInfos_, -1, -1},
	{"Eras", getNumEraInfos_, -1, -1},
	{"Policies", getNumPolicyInfos_, -1, -1},
	{"PolicyBranchTypes", getNumPolicyBranchInfos_, -1, -1},
	{"UnitPromotions", getNumPromotionInfos_, -1, -1},
	{"Improvements", getNumImprovementInfos_, -1, -1},
	{"Builds", getNumBuildInfos_, -1, -1},
	{"Routes", getNumRouteInfos_, -1, -1},
	{"Resources", getNumResourceInfos_, -1, -1},
	{"ResourceClasses", getNumResourceClassInfos_, -1, -1},
	{"Features", getNumFeatureInfos_, -1, -1},
	{"Terrains", getNumTerrainInfos_, -1, -1},
	{"Worlds", getNumWorldInfos_, -1, -1},
	{"Climates", getNumClimateInfos_, -1, -1},
	{"SeaLevels", getNumSeaLevelInfos_, -1, -1},
	{"Specialists", getNumSpecialistInfos_, -1, -1},
	{"HandicapInfos", getNumHandicapInfos_, -1, -1},
	{"GameSpeeds", getNumGameSpeedInfos_, -1, -1},
	{"Victories", getNumVictoryInfos_, -1, -1},
	{"Beliefs", getNumBeliefInfos_, -1, -1},
	{"Religions", getNumReligionInfos_, -1, -1},
	{"Traits", getNumTraitInfos_, -1, -1},
	{"Projects", getNumProjectInfos_, -1, -1},
	{"Processes", getNumProcessInfos_, -1, -1},
	{"GameOptions", getNumGameOptionInfos_, -1, -1},
	{"Resolutions", getNumResolutionInfos_, -1, -1},
	{"LeagueProjects", getNumLeagueProjectInfos_, -1, -1},
};
const unsigned kNumKeyTables = sizeof(kKeyTables) / sizeof(kKeyTables[0]);
const int kCivhostDlcAllowed = 14;

}  // namespace

extern "C" int rt_selftest_load(const gc_host* host, char* report, size_t report_len)
{
	SetLogSink(ForwardLog, const_cast<gc_host*>(host));
	Check c;
	// Left alive on purpose: GC.init's objects keep statements on the DB connection (civhost never unloads either).
	RtSession* session = new RtSession();
	RtSession::LoadStatus st;
	const bool loaded = session->Load(*host, st);
	c.add("load", loaded, "boot D1-D2 B1-B4 I1-I8 reached step %s%s%s", RtSession::StepName(st.step), st.error.empty() ? "" : ": ", st.error.c_str());

	std::string tables;
	int dbTables = -1;
	if (loaded)
	{
		Database::Connection& db = *session->Db().Connection();
		dbTables = session->Db().TableCount();
		c.add("table_count", dbTables == 427, "%d tables (expect 427)", dbTables);
		c.add("i7_database_utility", st.flush && st.postProcess && st.cache, "flush=%d postprocess=%d cache=%d (civhost 1/1/1)", st.flush, st.postProcess, st.cache);
		c.add("i8_dlc_allowed", st.dlcAllowed == kCivhostDlcAllowed, "%d DLC packages allowed (civhost %d)", st.dlcAllowed, kCivhostDlcAllowed);
		c.add("saved_game_db", rt_fs::Exists("./Civ5SavedGameDatabase.db"), "Civ5SavedGameDatabase.db created by CvGame::reset (served by rt_fs)");

		CvDllGameContext* ctx = session->Context();
		const int civs = ctx->GetNumCivilizationInfos(), units = ctx->GetNumUnitInfos();
		const int minors = ctx->GetNumMinorCivInfos(), leaders = ctx->GetNumLeaderHeadInfos();
		c.add("ctx_infos", civs == 116 && units == 265 && minors == 59 && leaders == 115,
			"GetNumCivilizationInfos=%d GetNumUnitInfos=%d GetNumMinorCivInfos=%d GetNumLeaderHeadInfos=%d (civhost 116/265/59/115)", civs, units, minors, leaders);

		int mismatched = 0;
		for (unsigned i = 0; i < kNumKeyTables; ++i)
		{
			const KeyTable& k = kKeyTables[i];
			const int rows = session->Db().Count(k.table);
			const int maxId1 = ScalarInt(db, Format("select max(ID)+1 from %s", k.table).c_str());
			const int infos = k.infos();
			bool ok = rows > 0 && infos == maxId1;
			if (k.civhostRows >= 0) ok = ok && rows == k.civhostRows;
			if (k.civhostInfos >= 0) ok = ok && infos == k.civhostInfos;
			if (!ok) ++mismatched;
			tables += Format("%s{\"table\":\"%s\",\"rows\":%d,\"max_id_plus_1\":%d,\"gc_infos\":%d,\"civhost_rows\":%d,\"civhost_infos\":%d,\"ok\":%s}",
				tables.empty() ? "" : ",", k.table, rows, maxId1, infos, k.civhostRows, k.civhostInfos, ok ? "true" : "false");
			printf("load: %-4s %-18s rows=%-4d max(ID)+1=%-4d infos=%-4d civhost rows=%d infos=%d\n", ok ? "ok" : "FAIL", k.table, rows, maxId1, infos,
				k.civhostRows, k.civhostInfos);
		}
		c.add("key_tables", mismatched == 0, "%d of %u key tables differ", mismatched, kNumKeyTables);

		const int warrior = GC.getInfoTypeForString("UNIT_WARRIOR", true);
		CvUnitEntry* pkWarrior = warrior >= 0 ? GC.getUnitInfo((UnitTypes)warrior) : NULL;
		c.add("unit_warrior", pkWarrior != NULL && pkWarrior->GetCombat() > 0, "GC.getUnitInfo(UNIT_WARRIOR): id=%d combat=%d", warrior,
			pkWarrior ? pkWarrior->GetCombat() : -1);
		Database::Results def;
		const bool haveDefine = db.Execute(def, "select Value from Defines where Name = 'MAX_CITY_HIT_POINTS'") && def.Step();
		const int dbHp = haveDefine ? def.GetInt(0) : -1;
		c.add("defines", haveDefine && GC.getMAX_CITY_HIT_POINTS() == dbHp, "GC.getMAX_CITY_HIT_POINTS()=%d, Defines=%d", GC.getMAX_CITY_HIT_POINTS(), dbHp);
	}

	std::string out = Format("{\"step\":\"%s\",\"error\":\"%s\",\"db_tables\":%d,\"dlc_allowed\":%d,\"failed\":%d,\"key_tables\":[", RtSession::StepName(st.step),
		Escape(st.error).c_str(), dbTables, st.dlcAllowed, c.failed);
	out += tables + "],\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	SetLogSink(NULL, NULL);
	return c.failed == 0 ? 0 : 1;
}
