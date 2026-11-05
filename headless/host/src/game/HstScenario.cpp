//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstScenario.cpp
//!  \brief     G1-G9. Reset prologue copied from exe Rng_SetupPregameSeeds_A (RVA 0xfbd0) and 0x7b760.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "game/HstScenario.h"
#include "game/HstJson.h"
#include "data/HstDatabase.h"
#include "platform/HstPlatform.h"

HstScenario::HstScenario()
	: m_uiSyncSeed(1)
	, m_uiMapSeed(1)
	, m_strGameType("single")
	, m_strWorldSize("WORLDSIZE_STANDARD")
	, m_strMapScript("Assets\\Maps\\Lekmap v5.2\\LekmapPangaeaFractalv5.2.lua")
	, m_strClimate("CLIMATE_TEMPERATE")
	, m_strSeaLevel("SEALEVEL_MEDIUM")
	, m_strEra("ERA_ANCIENT")
	, m_strGameSpeed("GAMESPEED_STANDARD")
	, m_strHandicap("HANDICAP_AI_DEFAULT")
	, m_iMaxTurns(0)
	, m_iActivePlayer(0)
	, m_bObserver(false)
	, m_strObserverCiv("CIVILIZATION_AMERICA")
	, m_bHasLuaContexts(false)
{
}

bool HstScenario::Load(const std::wstring& wstrPath, std::string& strError)
{
	HstJson kRoot;
	if(!HstJson::ParseFile(wstrPath, kRoot, strError))
		return false;
	return LoadJson(kRoot, strError);
}

void HstScenario::SetActivePlayer(const std::string& strValue)
{
	if(strValue == "observer")
		m_bObserver = true;
	else
	{
		m_bObserver = false;
		m_iActivePlayer = atoi(strValue.c_str());
	}
}

void HstScenario::SetGameType(const std::string& strValue)
{
	m_strGameType = (strValue == "network" || strValue == "network_mp") ? "network_mp" : "single";
}

// Game options of a typical Lekmod multiplayer lobby, in the order the real game keeps them. Source: the pregame block
// (archive.game_options = CvPreGame::s_GameOptions as written by CvPreGame.cpp saveTo << s_GameOptions) of the 95 real saves
// in headless/out/corpus: 55 of them hold exactly this list; the other lists are longer (up to 26 entries) and differ in the
// toggles the host chose. The real exe does NOT fill every row of the GameOptions table: a list never has more than 26 of the 66
// rows, every other option is answered per call from the table's Default column (CvPreGame::GetGameOption). Options whose
// lobby value differs from the table default: QUICK_COMBAT 1 (default 0; the host sets it anyway), QUICK_MOVEMENT 1 (0),
// AI_OPTIONS 1 (0), AI_GIMP_ALWAYS_WHITE_PEACE 0 (default 1), ADDITIONAL 1 (0), TWEAKED_SCORING 1 (0).
// Use it with game_type "network": CvGame::init drops SIMULTANEOUS_TURNS / DYNAMIC_TURNS / PITBOSS in a single-player game (then
// the saved list has SIMULTANEOUS_TURNS 0 and a trailing PITBOSS 0); with "network" the pregame option list of a checkpoint
// equals the modal list of the real saves entry by entry (checked on a turn-2 checkpoint of lekmod6).
static const struct { const char* szName; int iValue; } s_akLekmodMpOptions[] =
{
	{ "GAMEOPTION_DYNAMIC_TURNS", 0 },
	{ "GAMEOPTION_SIMULTANEOUS_TURNS", 1 },
	{ "GAMEOPTION_QUICK_COMBAT", 1 },
	{ "GAMEOPTION_QUICK_MOVEMENT", 1 },
	{ "GAMEOPTION_END_TURN_TIMER_ENABLED", 0 },
	{ "GAMEOPTION_AI_OPTIONS", 1 },
	{ "GAMEOPTION_AI_GIMP_ALWAYS_WHITE_PEACE", 0 },
	{ "GAMEOPTION_ADDITIONAL", 1 },
	{ "GAMEOPTION_TWEAKED_SCORING", 1 },
};

bool HstScenario::SetGameOptionsPreset(const std::string& strName, std::string& strError)
{
	if(strName.empty() || strName == "db" || strName == "none")
		m_strGameOptionsPreset.clear();
	else if(strName == "lekmod-mp")
		m_strGameOptionsPreset = strName;
	else
	{
		strError = "unknown game options preset '" + strName + "' (db | lekmod-mp)";
		return false;
	}
	return true;
}

bool HstScenario::LoadJson(const HstJson& kRoot, std::string& strError)
{
	if(kRoot.GetType() != HstJson::JSON_OBJECT)
	{
		strError = "scenario root must be an object";
		return false;
	}
	m_strName = kRoot.GetString("name", "unnamed");
	if(const HstJson* pkSeeds = kRoot.Get("seeds"))
	{
		m_uiSyncSeed = pkSeeds->GetUInt("sync", m_uiSyncSeed);
		m_uiMapSeed = pkSeeds->GetUInt("map", m_uiMapSeed);
	}
	SetGameType(kRoot.GetString("game_type", m_strGameType.c_str()));
	m_strWorldSize = kRoot.GetString("world_size", m_strWorldSize.c_str());
	m_strMapScript = kRoot.GetString("map_script", m_strMapScript.c_str());
	m_strClimate = kRoot.GetString("climate", m_strClimate.c_str());
	m_strSeaLevel = kRoot.GetString("sea_level", m_strSeaLevel.c_str());
	m_strEra = kRoot.GetString("era", m_strEra.c_str());
	m_strGameSpeed = kRoot.GetString("game_speed", m_strGameSpeed.c_str());
	m_strHandicap = kRoot.GetString("handicap", m_strHandicap.c_str());
	if(!SetGameOptionsPreset(kRoot.GetString("game_options_preset", ""), strError))
		return false;
	m_iMaxTurns = kRoot.GetInt("max_turns", m_iMaxTurns);
	if(const HstJson* pkActive = kRoot.Get("active_player"))
	{
		if(pkActive->GetType() == HstJson::JSON_STRING)
			SetActivePlayer(pkActive->AsString());
		else
			m_iActivePlayer = pkActive->AsInt(m_iActivePlayer);
	}
	m_strObserverCiv = kRoot.GetString("observer_civ", m_strObserverCiv.c_str());
	m_strHumanHandicap = kRoot.GetString("human_handicap", "HANDICAP_PRINCE");
	if(const HstJson* pkContexts = kRoot.Get("lua_contexts"))
	{
		m_bHasLuaContexts = true;
		for(size_t i = 0; i < pkContexts->Size(); ++i)
			m_aLuaContexts.push_back(pkContexts->At(i).AsString());
	}
	if(const HstJson* pkCivs = kRoot.Get("civs"))
	{
		for(size_t i = 0; i < pkCivs->Size(); ++i)
		{
			const HstJson& kCiv = pkCivs->At(i);
			m_aCivs.push_back(std::make_pair(kCiv.GetString("civ", ""), kCiv.GetString("leader", "")));
			m_abHuman.push_back(kCiv.GetBool("human", false));
		}
	}
	if(const HstJson* pkMinors = kRoot.Get("minors"))
	{
		for(size_t i = 0; i < pkMinors->Size(); ++i)
			m_aMinors.push_back(pkMinors->At(i).AsString());
	}
	if(const HstJson* pkOptions = kRoot.Get("map_options"))
	{
		for(size_t i = 0; i < pkOptions->Size(); ++i)
			m_aMapOptions.push_back(std::make_pair(pkOptions->KeyAt(i), pkOptions->ValueAt(i).AsInt()));
	}
	if(const HstJson* pkOptions = kRoot.Get("game_options"))
	{
		for(size_t i = 0; i < pkOptions->Size(); ++i)
			m_aGameOptions.push_back(std::make_pair(pkOptions->KeyAt(i), pkOptions->ValueAt(i).AsInt()));
	}
	if(m_aCivs.empty() || m_aCivs.size() > MAX_MAJOR_CIVS || m_aMinors.size() > MAX_MINOR_CIVS)
	{
		strError = "scenario needs 1..22 civs and at most 41 minors";
		return false;
	}
	return true;
}

static int HstTypeId(ICvGameContext1* pkCtx, const std::string& strType, const char* szWhat)
{
	const int iId = pkCtx->GetInfoTypeForString(strType.c_str(), true);
	if(iId < 0)
		HstLog("scenario: unknown %s '%s'", szWhat, strType.c_str());
	return iId;
}

bool HstScenario::ApplyPreGame(ICvGameContext1* pkCtx, HstDatabase* pkDb)
{
	std::auto_ptr<ICvPreGame1> pkPreGame(pkCtx->GetPreGame());

	// G1
	pkPreGame->resetSlots();
	pkPreGame->resetGame();
	pkPreGame->resetPlayers();
	pkPreGame->setSyncRandomSeed(m_uiSyncSeed);
	pkPreGame->setMapRandomSeed(m_uiMapSeed);

	// G2
	pkPreGame->setGameType(m_strGameType == "network_mp" ? GAME_NETWORK_MULTIPLAYER : GAME_SINGLE_PLAYER);

	// G3: with bResetSlots it sets 8 default SS_COMPUTER slots, so it goes before the slot setup.
	const int iWorld = HstTypeId(pkCtx, m_strWorldSize, "world size");
	if(iWorld < 0)
		return false;
	pkPreGame->setWorldSize((WorldSizeTypes)iWorld, true);

	// G4: must equal MapScriptOptions.FileName (GetMapOption falls back to that table).
	pkPreGame->setMapScriptName(CvString(m_strMapScript));

	// G5: every option of the script explicitly (defaults, then overrides).
	m_aAppliedMapOptions.clear();
	{
		Database::Results kOptions;
		if(pkDb->GetConnection()->Execute(kOptions, "SELECT OptionID, DefaultValue FROM MapScriptOptions WHERE FileName = ? ORDER BY rowid"))
		{
			kOptions.Bind(1, m_strMapScript.c_str());
			while(kOptions.Step())
			{
				const char* szId = kOptions.GetText(0);
				if(szId)
					m_aAppliedMapOptions.push_back(std::make_pair(std::string(szId), kOptions.GetInt(1)));
			}
		}
		for(size_t i = 0; i < m_aMapOptions.size(); ++i)
		{
			bool bFound = false;
			for(size_t j = 0; j < m_aAppliedMapOptions.size(); ++j)
			{
				if(m_aAppliedMapOptions[j].first == m_aMapOptions[i].first)
				{
					m_aAppliedMapOptions[j].second = m_aMapOptions[i].second;
					bFound = true;
				}
			}
			if(!bFound)
				m_aAppliedMapOptions.push_back(m_aMapOptions[i]);
		}
		if(m_aAppliedMapOptions.empty())
			HstLog("scenario: WARNING no MapScriptOptions rows for '%s'", m_strMapScript.c_str());
		for(size_t i = 0; i < m_aAppliedMapOptions.size(); ++i)
			pkPreGame->SetMapOption(m_aAppliedMapOptions[i].first.c_str(), m_aAppliedMapOptions[i].second);
	}

	// G6
	const int iClimate = HstTypeId(pkCtx, m_strClimate, "climate");
	const int iSeaLevel = HstTypeId(pkCtx, m_strSeaLevel, "sea level");
	const int iEra = HstTypeId(pkCtx, m_strEra, "era");
	const int iSpeed = HstTypeId(pkCtx, m_strGameSpeed, "game speed");
	const int iHandicap = HstTypeId(pkCtx, m_strHandicap, "handicap");
	if(iClimate < 0 || iSeaLevel < 0 || iEra < 0 || iSpeed < 0 || iHandicap < 0)
		return false;
	pkPreGame->setClimate((ClimateTypes)iClimate);
	pkPreGame->setSeaLevel((SeaLevelTypes)iSeaLevel);
	pkPreGame->setEra((EraTypes)iEra);
	pkPreGame->setGameSpeed((GameSpeedTypes)iSpeed);
	if(m_strGameOptionsPreset == "lekmod-mp")
	{
		for(size_t i = 0; i < HST_ARRAY_SIZE(s_akLekmodMpOptions); ++i)
			pkPreGame->SetGameOption(s_akLekmodMpOptions[i].szName, s_akLekmodMpOptions[i].iValue);
	}
	for(size_t i = 0; i < m_aGameOptions.size(); ++i)
		pkPreGame->SetGameOption(m_aGameOptions[i].first.c_str(), m_aGameOptions[i].second);
	pkPreGame->setMaxTurns(m_iMaxTurns);
	pkPreGame->setQuickCombat(true);
	pkPreGame->setQuickMovement(true);

	// G7: explicit civs (the exe's random choice, RVA 0x10b090, is not reproduced).
	for(int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const PlayerTypes eSlot = (PlayerTypes)i;
		if(i < (int)m_aCivs.size())
		{
			const int iCiv = HstTypeId(pkCtx, m_aCivs[i].first, "civilization");
			const int iLeader = HstTypeId(pkCtx, m_aCivs[i].second, "leader");
			if(iCiv < 0 || iLeader < 0)
				return false;
			// "human": true = a human slot (SS_TAKEN, isHuman() on every peer); in lockstep runs a bot on the peer whose active
			// player it is ends its turns (civsim.lockstep "bots"); without one the turn waits for its NetTurnComplete.
			const bool bHuman = i < (int)m_abHuman.size() && m_abHuman[i];
			int iSlotHandicap = iHandicap;
			if(bHuman)
			{
				iSlotHandicap = HstTypeId(pkCtx, m_strHumanHandicap, "human handicap");
				if(iSlotHandicap < 0)
					return false;
			}
			pkPreGame->setSlotStatus(eSlot, bHuman ? SS_TAKEN : SS_COMPUTER);
			pkPreGame->setSlotClaim(eSlot, SLOTCLAIM_ASSIGNED);
			pkPreGame->setCivilization(eSlot, (CivilizationTypes)iCiv);
			pkPreGame->setLeaderHead(eSlot, (LeaderHeadTypes)iLeader);
			pkPreGame->setTeamType(eSlot, (TeamTypes)i);
			pkPreGame->setHandicap(eSlot, (HandicapTypes)iSlotHandicap);
		}
		else
			pkPreGame->setSlotStatus(eSlot, SS_CLOSED);
	}

	// G8: mandatory, InitPlayers dereferences GC.getMinorCivInfo(minorCivType(p)) (CvGame.cpp:750-757).
	pkPreGame->setNumMinorCivs((int)m_aMinors.size());
	for(size_t j = 0; j < m_aMinors.size(); ++j)
	{
		const int iMinor = HstTypeId(pkCtx, m_aMinors[j], "minor civ");
		if(iMinor < 0)
			return false;
		pkPreGame->setMinorCivType((PlayerTypes)(MAX_MAJOR_CIVS + j), (MinorCivTypes)iMinor);
	}

	// R9: observer = an SS_OBSERVER major slot right after the civs (as an MP observer peer: claim assigned). InitPlayers
	// moves it to OBSERVER_TEAM and reads its civilization's default color, so it needs a valid civilization.
	if(m_bObserver)
	{
		const int iSlot = (int)m_aCivs.size();
		const int iCiv = HstTypeId(pkCtx, m_strObserverCiv, "observer civilization");
		if(iSlot >= MAX_MAJOR_CIVS || iCiv < 0)
		{
			HstLog("scenario: no free major slot / civilization for the observer");
			return false;
		}
		const int iHandicap2 = HstTypeId(pkCtx, m_strHandicap, "handicap");
		pkPreGame->setSlotStatus((PlayerTypes)iSlot, SS_OBSERVER);
		pkPreGame->setSlotClaim((PlayerTypes)iSlot, SLOTCLAIM_ASSIGNED);
		pkPreGame->setCivilization((PlayerTypes)iSlot, (CivilizationTypes)iCiv);
		pkPreGame->setTeamType((PlayerTypes)iSlot, (TeamTypes)iSlot);
		pkPreGame->setHandicap((PlayerTypes)iSlot, (HandicapTypes)iHandicap2);
		m_iActivePlayer = iSlot;
	}

	// G9
	pkPreGame->setActivePlayer((PlayerTypes)m_iActivePlayer);
	return true;
}

std::string HstScenario::Describe() const
{
	char szBuf[512];
	std::string str;
	_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, "scenario '%s': seeds sync=%u map=%u, %s, %s, %s\n", m_strName.c_str(), m_uiSyncSeed, m_uiMapSeed,
		m_strGameType.c_str(), m_strWorldSize.c_str(), m_strMapScript.c_str());
	str += szBuf;
	_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, "  %s %s %s %s, ai %s, max_turns %d, active %d%s\n", m_strClimate.c_str(), m_strSeaLevel.c_str(),
		m_strEra.c_str(), m_strGameSpeed.c_str(), m_strHandicap.c_str(), m_iMaxTurns, m_iActivePlayer, m_bObserver ? " (observer)" : "");
	str += szBuf;
	str += "  civs:";
	for(size_t i = 0; i < m_aCivs.size(); ++i)
		str += " " + m_aCivs[i].first + "/" + m_aCivs[i].second;
	str += "\n  minors:";
	for(size_t i = 0; i < m_aMinors.size(); ++i)
		str += " " + m_aMinors[i];
	str += "\n  map options:";
	for(size_t i = 0; i < m_aAppliedMapOptions.size(); ++i)
	{
		_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, " %s=%d", m_aAppliedMapOptions[i].first.c_str(), m_aAppliedMapOptions[i].second);
		str += szBuf;
	}
	if(!m_strGameOptionsPreset.empty())
	{
		_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, "\n  game options preset: %s (+ %d scenario override(s)); all other options use the GameOptions table default",
			m_strGameOptionsPreset.c_str(), (int)m_aGameOptions.size());
		str += szBuf;
	}
	return str;
}
