//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstGameRunner.h
//!  \brief     Launch (design 1.7, L1-L6, L8), map summary/digest and the turn loop (design 1.8).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTGAMERUNNER_H
#define HSTGAMERUNNER_H

#include "HstCommon.h"

class HstEngineUtility;

//! Result of one turn of the loop.
struct HstTurnResult
{
	int iFromTurn;				// GetGameTurn() before
	int iToTurn;				// GetGameTurn() after (iFromTurn + 1 unless stuck)
	unsigned int uiSlices;		// ICvGame1::Update calls
	int iNetMessages;			// loopback messages delivered
	double dMs;					// wall time of the Update calls
	bool bStuck;				// turn did not advance within the slice cap
	unsigned int uiFpuWord;		// x87 control word after the turn
};

class HstGameRunner
{
public:
	explicit HstGameRunner(ICvGameContext1* pkCtx) : m_pkCtx(pkCtx) {}

	//! L1-L6: cache, closeInactiveSlots, Init(handicap), Init2 (map generation), tactical map, initial time.
	bool LaunchToMap();
	//! L8: ICvGame1::DoGameStarted (exe NetPlayerReady_Execute 0x80c80 -> +0x2c).
	void StartGame();
	//! Design 1.8: Update() until GetGameTurn() changes (cap iMaxSlices). Before every Update the queued
	//! loopback messages are delivered and the virtual clock advances by one slice (getMillisecsPerTurn).
	//! pfnSlice(pUser) is called before every Update (control channel keep-alive); may be NULL.
	HstTurnResult RunTurn(HstEngineUtility* pkEngine, int iMaxSlices, void (*pfnSlice)(void*) = NULL, void* pUser = NULL);
	//! Lockstep peer frame: one slice exactly as RunTurn runs it (virtual clock, stack wipe, FPU, Update), with the received
	//! messages delivered first (peer mode: the coordinator's inbox instead of the local loopback queue). Returns the Update
	//! wall time in ms. pkGame: the caller's game wrapper, kept for the whole turn as RunTurn does.
	double RunSlice(ICvGame1* pkGame, HstEngineUtility* pkEngine, const std::vector<struct HstNetMessage>& akInbox);
	//! L5b again after a load (the tactical analysis map is not serialized).
	void InitTacticalMap();
	int GetGameTurn();
	//! FNV-1a 64 of ICvMap1::Write (the DLL's own serialization of the whole map).
	unsigned __int64 MapDigest();
	//! Log plot-type / terrain / feature histograms.
	void LogMapSummary();

	static unsigned __int64 Fnv1a64(const void* pData, size_t uiBytes, unsigned __int64 ui64Seed = 14695981039346656037ULL);

protected:
	ICvGameContext1* m_pkCtx;
};

#endif // HSTGAMERUNNER_H
