//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstScenario.h
//!  \brief     Scenario JSON -> pregame (design 1.6, G1-G9). Seeds come only from the scenario.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTSCENARIO_H
#define HSTSCENARIO_H

#include "HstCommon.h"

class HstDatabase;

class HstScenario
{
public:
	HstScenario();

	bool Load(const std::wstring& wstrPath, std::string& strError);
	//! From an already parsed JSON object (control channel "new_game" with an inline scenario).
	bool LoadJson(const class HstJson& kRoot, std::string& strError);
	//! "--active-player <slot|observer>" / control override.
	void SetActivePlayer(const std::string& strValue);
	//! "--game-type single|network" override.
	void SetGameType(const std::string& strValue);
	//! "game_options_preset" / "--game-options-preset": "" or "db" (default: only the scenario's own game_options, everything
	//! else answered by the GameOptions table defaults, as in the real game) or "lekmod-mp" (the option list of a typical Lekmod
	//! multiplayer lobby, see HstScenario.cpp). false + strError for an unknown name.
	bool SetGameOptionsPreset(const std::string& strName, std::string& strError);
	//! G1-G9 on ICvPreGame1. Map options: all MapScriptOptions defaults of the script, then scenario overrides.
	bool ApplyPreGame(ICvGameContext1* pkCtx, HstDatabase* pkDb);
	//! One line per setting, for the log and the scenario hash.
	std::string Describe() const;

	std::string m_strName;
	unsigned int m_uiSyncSeed;
	unsigned int m_uiMapSeed;
	std::string m_strGameType;		// "single" | "network_mp"
	std::string m_strWorldSize;
	std::string m_strMapScript;
	std::string m_strClimate;
	std::string m_strSeaLevel;
	std::string m_strEra;
	std::string m_strGameSpeed;
	std::string m_strHandicap;		// AI slots (InitPlayers forces AI_HANDICAP for SS_COMPUTER anyway)
	int m_iMaxTurns;
	int m_iActivePlayer;			// slot of the active player (with m_bObserver: the observer slot, set by ApplyPreGame)
	bool m_bObserver;				// "active_player": "observer" (design R9): an SS_OBSERVER slot after the civs is active
	std::string m_strObserverCiv;	// civilization given to the observer slot (InitPlayers reads its default color)
	bool m_bHasLuaContexts;			// "lua_contexts" present: overrides the list parsed from InGame.lua
	std::vector<std::string> m_aLuaContexts;
	std::vector<std::pair<std::string, std::string> > m_aCivs;	// civilization type, leader type
	std::vector<bool> m_abHuman;								// per civ: "human": true -> SS_TAKEN slot (lockstep bot slot)
	std::string m_strHumanHandicap;								// "human_handicap" (default HANDICAP_PRINCE)
	std::vector<std::string> m_aMinors;							// minor civ types
	std::vector<std::pair<std::string, int> > m_aMapOptions;	// overrides (OptionID -> value)
	std::vector<std::pair<std::string, int> > m_aGameOptions;
	std::string m_strGameOptionsPreset;	// "" (none) | "lekmod-mp"
	std::vector<std::pair<std::string, int> > m_aAppliedMapOptions;	// filled by ApplyPreGame
};

#endif // HSTSCENARIO_H
