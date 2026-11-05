//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstGameRunner.cpp
//!  \brief     L1-L6, mirroring exe SPLaunch 0x18cbc0 -> LaunchGame 0x370820 (GameInit 0x75c40, Init2 0x370aef).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "game/HstGameRunner.h"
#include "platform/HstPlatform.h"
#include "engine/HstEngineUtility.h"
#include "platform/HstDetHeap.h"
#include "diag/HstRngLog.h"

static const unsigned int s_uiHstWipeBytes = 256 * 1024;	// below the runner's frame, before every DLL entry

unsigned __int64 HstGameRunner::Fnv1a64(const void* pData, size_t uiBytes, unsigned __int64 ui64Hash)
{
	const unsigned char* puc = (const unsigned char*)pData;
	for(size_t i = 0; i < uiBytes; ++i)
	{
		ui64Hash ^= puc[i];
		ui64Hash *= 1099511628211ULL;
	}
	return ui64Hash;
}

bool HstGameRunner::LaunchToMap()
{
	// L1 (no-op when already cached)
	{
		std::auto_ptr<ICvDLLDatabaseUtility1> pkLoader(m_pkCtx->GetDatabaseLoadUtility());
		pkLoader->CacheGameDatabaseData();
	}
	std::auto_ptr<ICvPreGame1> pkPreGame(m_pkCtx->GetPreGame());
	// L2: sets s_gameStarted; BeginSendBundle/sendPlayerInfo/EndSendBundle are sinks (CvPreGame.cpp:817).
	pkPreGame->closeInactiveSlots();

	// L3 (exe GameInit 0x75c40, decompiled 2026-10-17): the game handicap is the integer average of the handicaps of slots
	// 0..62 whose status is SS_TAKEN or SS_OPEN (pregame +0x2fc == 3 || == 0, handicap +0xb0); STANDARD_HANDICAP (ctx +0x13c)
	// when there is none. It depends on the pregame only, so every peer computes the same value. (Earlier GUESS: the active
	// player's handicap if its slot is taken - identical for all-AI scenarios, wrong for observers / mixed human handicaps.)
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	int iHandicapSum = 0, iHandicapSlots = 0;
	for(int i = 0; i < MAX_PLAYERS - 1; ++i)
	{
		const SlotStatus eStatus = pkPreGame->slotStatus((PlayerTypes)i);
		if(eStatus == SS_TAKEN || eStatus == SS_OPEN)
		{
			iHandicapSum += (int)pkPreGame->handicap((PlayerTypes)i);
			++iHandicapSlots;
		}
	}
	const HandicapTypes eHandicap = iHandicapSlots > 0 ? (HandicapTypes)(iHandicapSum / iHandicapSlots) : (HandicapTypes)m_pkCtx->GetSTANDARD_HANDICAP();
	HstLog("launch: game handicap %d (%d taken/open slot(s))", (int)eHandicap, iHandicapSlots);
	HstLog("launch: game->Init(handicap %d)", (int)eHandicap);
	HstWipeStack(s_uiHstWipeBytes);
	pkGame->Init(eHandicap);

	// L4: InitPlayers, InitMap (Lua map script), initDiplomacy, setInitialItems, ...
	HstLog("launch: game->Init2() ...");
	DWORD dwStart = GetTickCount();
	HstWipeStack(s_uiHstWipeBytes);
	const bool bInit2 = pkGame->Init2();
	HstLog("launch: game->Init2() returned %d (%u ms)", (int)bInit2, (unsigned int)(GetTickCount() - dwStart));

	// SPLaunch 0x18cbc0 then calls exe 0x33ef70: Lua event "PreGameStart" (M4), then 0x2d5c30:
	// L5a: ICvPlayer1::LaterInit (+0x10) for all 64 players -> CvPlayer::gameStartInit (danger plots, InitPlots).
	//      Without it CvDangerPlots::GetDanger crashes in the first AI_unitUpdate.
	HstWipeStack(s_uiHstWipeBytes);
	for(int i = 0; i < MAX_PLAYERS; ++i)
	{
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer((PlayerTypes)i));
		if(pkPlayer.get())
			pkPlayer->LaterInit();
	}
	// L5b: same exe function, right after the LaterInit loop: InitTacticalAnalysisMap(map->NumPlots()) (+0xa4).
	std::auto_ptr<ICvMap1> pkMap(m_pkCtx->GetMap());
	pkGame->InitTacticalAnalysisMap(pkMap->NumPlots());
	// L6: virtual time (GUESS: exe call site not found)
	pkGame->SetInitialTime(0);
	return bInit2;
}

void HstGameRunner::StartGame()
{
	// L8, as exe NetPlayerReady_Execute 0x80c80 once every player is ready: DoGameStarted (+0x2c),
	// SendPlayerOptions(true) (+0xf8), then the network synchronizer's Clear*Deltas (city, random, player, plot, unit).
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	HstLog("launch: game->DoGameStarted() (turn %d)", pkGame->GetGameTurn());
	HstWipeStack(s_uiHstWipeBytes);
	pkGame->DoGameStarted();
	pkGame->SendPlayerOptions(true);
	ICvNetworkSyncronization1* pkSync = m_pkCtx->GetNetworkSyncronizer();
	if(pkSync)
	{
		pkSync->ClearCityDeltas();
		pkSync->ClearRandomDeltas();
		pkSync->ClearPlayerDeltas();
		pkSync->ClearPlotDeltas();
		pkSync->ClearUnitDeltas();
	}
}

int HstGameRunner::GetGameTurn()
{
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	return pkGame->GetGameTurn();
}

void HstGameRunner::InitTacticalMap()
{
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	std::auto_ptr<ICvMap1> pkMap(m_pkCtx->GetMap());
	HstWipeStack(s_uiHstWipeBytes);
	pkGame->InitTacticalAnalysisMap(pkMap->NumPlots());
}

HstTurnResult HstGameRunner::RunTurn(HstEngineUtility* pkEngine, int iMaxSlices, void (*pfnSlice)(void*), void* pUser)
{
	std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
	HstTurnResult kResult;
	kResult.iFromTurn = pkGame->GetGameTurn();
	kResult.iToTurn = kResult.iFromTurn;
	kResult.uiSlices = 0;
	kResult.iNetMessages = 0;
	kResult.bStuck = false;
	const unsigned int uiSliceMs = (unsigned int)pkEngine->getMillisecsPerTurn();

	LARGE_INTEGER kFreq, kStart, kEnd;
	QueryPerformanceFrequency(&kFreq);
	QueryPerformanceCounter(&kStart);
	HstRngLog::BeginPhase(kResult.iFromTurn);	// --rng-digest / --rng-log (no-op unless the diag sink is installed)
	while(pkGame->GetGameTurn() == kResult.iFromTurn)
	{
		if((int)kResult.uiSlices >= iMaxSlices)
		{
			kResult.bStuck = true;
			break;
		}
		if(pfnSlice)
			pfnSlice(pUser);
		kResult.iNetMessages += pkEngine->DispatchNetMessages();
		HstVirtualTimeAdvance(uiSliceMs);
		HstWipeStack(s_uiHstWipeBytes);	// stack residue must come from the DLL's own call history (HstPlatform.h)
		HstFpuApplyTurn();				// --fpu-pc (no-op by default)
		HstRngLog::NextSlice();
		pkGame->Update();
		HST_FPU_CHECK("Update slice");	// --fpu-check
		HstFpuEndTurnSlice();
		++kResult.uiSlices;
	}
	QueryPerformanceCounter(&kEnd);
	kResult.iToTurn = pkGame->GetGameTurn();
	HstRngLog::EndPhase();
	kResult.dMs = (double)(kEnd.QuadPart - kStart.QuadPart) * 1000.0 / (double)kFreq.QuadPart;
	kResult.uiFpuWord = HstGetFpuControlWord();
	return kResult;
}

double HstGameRunner::RunSlice(ICvGame1* pkGame, HstEngineUtility* pkEngine, const std::vector<HstNetMessage>& akInbox)
{
	pkEngine->SetHostTurn(pkGame->GetGameTurn());
	const unsigned int uiSliceMs = (unsigned int)pkEngine->getMillisecsPerTurn();
	LARGE_INTEGER kFreq, kStart, kEnd;
	QueryPerformanceFrequency(&kFreq);
	QueryPerformanceCounter(&kStart);
	for(size_t i = 0; i < akInbox.size(); ++i)
		pkEngine->DeliverRemote(akInbox[i]);
	pkEngine->DispatchNetMessages();		// local loopback queue (empty in peer mode unless a host-side inject used it)
	HstVirtualTimeAdvance(uiSliceMs);
	HstWipeStack(s_uiHstWipeBytes);
	HstFpuApplyTurn();
	HstRngLog::NextSlice();
	pkGame->Update();
	HST_FPU_CHECK("Update slice");
	HstFpuEndTurnSlice();
	pkEngine->ExeFrame();		// turn_model exe: the exe's between-updates turn logic (no-op otherwise)
	pkEngine->TurnSliceFrame();	// turn slices: NetTurnSlice when nobody is busy (no-op unless active)
	QueryPerformanceCounter(&kEnd);
	return (double)(kEnd.QuadPart - kStart.QuadPart) * 1000.0 / (double)kFreq.QuadPart;
}

unsigned __int64 HstGameRunner::MapDigest()
{
	std::auto_ptr<ICvMap1> pkMap(m_pkCtx->GetMap());
	FMemoryStream kStream;
	pkMap->Write(kStream);
	{
		HstDetHeapPauseScope kPause;
		FILE* pkFile = _wfopen(HstJoinPath(HstGetRunDir(), L"map-write.bin").c_str(), L"wb");
		if(pkFile)
		{
			fwrite(kStream.GetBuf(), 1, kStream.GetEOF(), pkFile);
			fclose(pkFile);
		}
	}
	return Fnv1a64(kStream.GetBuf(), kStream.GetEOF());
}

void HstGameRunner::LogMapSummary()
{
	std::auto_ptr<ICvMap1> pkMap(m_pkCtx->GetMap());
	const int iPlots = pkMap->NumPlots();
	HstLog("map: %d x %d = %d plots, wrapX=%d wrapY=%d", pkMap->GetGridWidth(), pkMap->GetGridHeight(), iPlots, (int)pkMap->IsWrapX(), (int)pkMap->IsWrapY());

	int aiPlotTypes[NUM_PLOT_TYPES] = {0};
	std::map<int, int> kTerrains, kFeatures, kResources;
	for(int i = 0; i < iPlots; ++i)
	{
		std::auto_ptr<ICvPlot1> pkPlot(pkMap->GetPlotByIndex(i));
		if(!pkPlot.get())
			continue;
		const int iType = (int)pkPlot->GetPlotType();
		if(iType >= 0 && iType < NUM_PLOT_TYPES)
			++aiPlotTypes[iType];
		++kTerrains[(int)pkPlot->GetTerrainType()];
		++kFeatures[(int)pkPlot->GetFeatureType()];
		const int iResource = (int)pkPlot->GetResourceType(NO_TEAM);
		if(iResource >= 0)
			++kResources[iResource];
	}
	HstLog("map: plot types mountain=%d hills=%d land=%d ocean=%d", aiPlotTypes[PLOT_MOUNTAIN], aiPlotTypes[PLOT_HILLS], aiPlotTypes[PLOT_LAND], aiPlotTypes[PLOT_OCEAN]);
	std::string strLine = "map: terrains";
	for(std::map<int, int>::const_iterator it = kTerrains.begin(); it != kTerrains.end(); ++it)
	{
		std::auto_ptr<ICvTerrainInfo1> pkInfo(it->first >= 0 ? m_pkCtx->GetTerrainInfo((TerrainTypes)it->first) : NULL);
		char szBuf[96];
		_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, " %s=%d", pkInfo.get() ? pkInfo->GetType() : "NONE", it->second);
		strLine += szBuf;
	}
	HstLog("%s", strLine.c_str());
	strLine = "map: features";
	for(std::map<int, int>::const_iterator it = kFeatures.begin(); it != kFeatures.end(); ++it)
	{
		std::auto_ptr<ICvFeatureInfo1> pkInfo(it->first >= 0 ? m_pkCtx->GetFeatureInfo((FeatureTypes)it->first) : NULL);
		char szBuf[96];
		_snprintf_s(szBuf, sizeof(szBuf), _TRUNCATE, " %s=%d", pkInfo.get() ? pkInfo->GetType() : "NONE", it->second);
		strLine += szBuf;
	}
	HstLog("%s", strLine.c_str());
	int iResourcePlots = 0;
	for(std::map<int, int>::const_iterator it = kResources.begin(); it != kResources.end(); ++it)
		iResourcePlots += it->second;
	HstLog("map: %d plots with resources (%d resource types)", iResourcePlots, (int)kResources.size());
}
