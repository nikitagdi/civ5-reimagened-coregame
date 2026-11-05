//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstUserInterface.h
//!  \brief     ICvUserInterface2 for the DLL. All methods are sink/query (engine-api.csv); overrides go here
//!				when a UI query turns out to steer the simulation.
//!
//!		Lockstep bot mode (peer turn_model "exe"): the local UI state a human's client has - the unit/city selection that
//!		Game.SelectionListGameNetMessage / SelectedCitiesGameNetMessage read (CvGame.cpp selectionListGameNetMessage) and
//!		the end-turn flag the DLL sets in updateTestEndTurn (Game.DoControl(CONTROL_ENDTURN) needs canEndTurn()). The MP
//!		auto end turn option is off (the bot ends its turns). Single mode and the legacy turn model keep the generated stubs.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTUSERINTERFACE_H
#define HSTUSERINTERFACE_H

#include "engine/gen/HstUserInterface2Gen.h"
#include "engine/HstEngineUtility.h"
#include "platform/HstDetHeap.h"

//! Lockstep bot mode: the enumerator GetSelectedUnits returns (the bot's selection, at most one unit). The real client's
//! InGame.lua calls Player:GetRecommendedWorkerPlots / GetRecommendedFoundCityPlots on every unit selection change, and both
//! read the selection through GetSelectedUnits (CvLuaPlayer.cpp); with the generated NULL they returned early. Ref counted
//! like the DLL's CvDllUnit (QueryInterface increments, Destroy decrements).
class HstSelectionEnumerator : public ICvEnumerator
{
public:
	HstSelectionEnumerator(ICvGameContext1* pkCtx, PlayerTypes eOwner, int iID) : m_pkCtx(pkCtx), m_eOwner(eOwner), m_iID(iID), m_iPos(-1), m_uiRef(1) {}
	void* operator new(size_t n) { return ::operator new(n); }
	void operator delete(void* p) { ::operator delete(p); }
	virtual void* DLLCALL QueryInterface(GUID g)
	{
		if(g == ICvUnknown::GetInterfaceId() || g == ICvEnumerator::GetInterfaceId()) { ++m_uiRef; return this; }
		return NULL;
	}
	virtual bool DLLCALL MoveNext() { return ++m_iPos == 0; }
	virtual void DLLCALL Reset() { m_iPos = -1; }
	virtual ICvUnknown* DLLCALL GetCurrent()
	{
		if(m_iPos != 0 || !m_pkCtx)
			return NULL;
		std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer(m_eOwner));
		return pkPlayer.get() ? pkPlayer->GetUnit(m_iID) : NULL;
	}
protected:
	virtual void DLLCALL Destroy() { if(--m_uiRef == 0) { HstDetHeapPauseScope kPause; delete this; } }
	ICvGameContext1* m_pkCtx;
	PlayerTypes m_eOwner;
	int m_iID;
	int m_iPos;
	unsigned int m_uiRef;
};

class HstUserInterface : public HstUserInterface2Gen
{
public:
	HstUserInterface() : m_pkCtx(NULL), m_pkEngine(NULL), m_bCanEndTurn(false), m_bHasUnit(false), m_bHasCity(false), m_bHasScratchDeal(false), m_uiScratchDeal(0)
	{
		m_kUnit.eOwner = NO_PLAYER; m_kUnit.iID = -1;
		m_kCity.eOwner = NO_PLAYER; m_kCity.iID = -1;
	}
	void SetContext(ICvGameContext1* pkCtx, HstEngineUtility* pkEngine) { m_pkCtx = pkCtx; m_pkEngine = pkEngine; }
	bool Live() const { return m_pkEngine && m_pkEngine->IsExeTurns(); }
	ICvGameContext1* GetContext() const { return m_pkCtx; }

	// host side (Lua UI.SelectUnit / UI.SelectCity / UI.ClearSelectionList of the bot)
	void HostSelectUnit(int iOwner, int iID) { m_kUnit.eOwner = (PlayerTypes)iOwner; m_kUnit.iID = iID; m_bHasUnit = iID >= 0; }
	void HostSelectCity(int iOwner, int iID) { m_kCity.eOwner = (PlayerTypes)iOwner; m_kCity.iID = iID; m_bHasCity = iID >= 0; }
	void HostClearSelection() { m_bHasUnit = false; m_bHasCity = false; }
	void ResetGame() { HostClearSelection(); m_bCanEndTurn = false; m_bHasScratchDeal = false; }

	//! Lockstep bot mode: the trade screen's scratch deal (exe: UI deal the DLL reads with GetScratchDeal, fills from AI offers
	//! with SetScratchDeal and the Lua trade screen edits through UI.GetScratchDeal()). One deal from ICvGameDeals1::CreateDeal
	//! (CvGameDeals::m_Deals is not serialized), created on first use with the deterministic heap paused (only peers with a
	//! local human UI create it). A fresh ref-counted wrapper per call, as the DLL wraps the result in an auto_ptr.
	ICvDeal1* HostScratchDeal()
	{
		if(!m_pkCtx)
			return NULL;
		HstDetHeapPauseScope kPause;
		std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
		std::auto_ptr<ICvGameDeals1> pkDeals(pkGame.get() ? pkGame->GetGameDeals() : NULL);
		if(!pkDeals.get())
			return NULL;
		if(!m_bHasScratchDeal)
		{
			m_uiScratchDeal = pkDeals->CreateDeal();
			m_bHasScratchDeal = true;
		}
		return pkDeals->GetDeal(m_uiScratchDeal);
	}
	virtual ICvDeal1* GetScratchDeal()
	{
		if(!Live())
			return HstUserInterface2Gen::GetScratchDeal();
		HST_API(kHstApi_UserInterface_GetScratchDeal);
		return HostScratchDeal();
	}
	virtual void SetScratchDeal(ICvDeal1* pDeal)
	{
		HstUserInterface2Gen::SetScratchDeal(pDeal);
		if(!Live() || !pDeal)
			return;
		std::auto_ptr<ICvDeal1> pkScratch(HostScratchDeal());
		if(pkScratch.get() && pkScratch.get() != pDeal)
		{
			HstDetHeapPauseScope kPause;	// AI offer to the local human (CvDiplomacyRequests::SendDealRequest): this peer only
			pkScratch->CopyFrom(pDeal);
		}
	}
	//! Leader messages (GameplayDiplomacyAILeaderMessage, the DLL calls it for the local human only): queued for the bot as
	//! popups of type kLeaderMessagePopup (Data1 = AI player, Data2 = DiploUIStateTypes, Data3 = iData1).
	enum { kLeaderMessagePopup = 10000 };
	void HostLeaderMessage(PlayerTypes ePlayer, int eDiploUIState, int iData1)
	{
		if(!Live() || m_akPopups.size() > 256)
			return;
		Popup k = { kLeaderMessagePopup, (int)ePlayer, eDiploUIState, iData1, 0, false, false };
		HstDetHeapPauseScope kPause;
		m_akPopups.push_back(k);
	}

	virtual ICvUnit1* GetHeadSelectedUnit()
	{
		if(!Live())
			return HstUserInterface2Gen::GetHeadSelectedUnit();
		HST_API(kHstApi_UserInterface_GetHeadSelectedUnit);
		if(!m_bHasUnit || !m_pkCtx)
			return NULL;
		std::auto_ptr<ICvGame1> pkGame(m_pkCtx->GetGame());
		std::auto_ptr<ICvPlayer1> pkPlayer(pkGame->GetPlayer(m_kUnit.eOwner));
		return pkPlayer.get() ? pkPlayer->GetUnit(m_kUnit.iID) : NULL;
	}
	virtual ICvEnumerator* GetSelectedUnits()
	{
		if(!Live())
			return HstUserInterface2Gen::GetSelectedUnits();
		HST_API(kHstApi_UserInterface_GetSelectedUnits);
		if(!m_bHasUnit || !m_pkCtx)
			return NULL;
		HstDetHeapPauseScope kPause;	// host object handed to the DLL (bot-only path, the bot runs paused anyway)
		return new HstSelectionEnumerator(m_pkCtx, m_kUnit.eOwner, m_kUnit.iID);
	}
	virtual void ClearSelectionList()
	{
		if(Live())
			m_bHasUnit = false;
		HstUserInterface2Gen::ClearSelectionList();
	}
	virtual int GetLengthSelectionList()
	{
		if(!Live())
			return HstUserInterface2Gen::GetLengthSelectionList();
		HST_API(kHstApi_UserInterface_GetLengthSelectionList);
		return m_bHasUnit ? 1 : 0;
	}
	virtual IDInfo* headSelectedCitiesNode()
	{
		if(!Live())
			return HstUserInterface2Gen::headSelectedCitiesNode();
		HST_API(kHstApi_UserInterface_headSelectedCitiesNode);
		return m_bHasCity ? &m_kCity : NULL;
	}
	virtual IDInfo* nextSelectedCitiesNode(IDInfo* pNode)
	{
		if(!Live())
			return HstUserInterface2Gen::nextSelectedCitiesNode(pNode);
		return NULL;
	}
	virtual const IDInfo* nextSelectedCitiesNode(const IDInfo* pNode) const
	{
		if(!Live())
			return HstUserInterface2Gen::nextSelectedCitiesNode(pNode);
		return NULL;
	}
	virtual bool isCitySelection()
	{
		if(!Live())
			return HstUserInterface2Gen::isCitySelection();
		return m_bHasCity;
	}
	virtual bool canEndTurn() const
	{
		if(!Live())
			return HstUserInterface2Gen::canEndTurn();
		return m_bCanEndTurn;
	}
	virtual void setCanEndTurn(bool bNewValue)
	{
		HstUserInterface2Gen::setCanEndTurn(bNewValue);
		m_bCanEndTurn = bNewValue;
	}
	virtual bool IsMPAutoEndTurnEnabled()
	{
		if(!Live())
			return HstUserInterface2Gen::IsMPAutoEndTurnEnabled();
		return false;
	}
	//! Popups the DLL raises for the local player (goody hut choice, free great person, found religion, ...): the real UI
	//! shows them and answers through Network.Send*; the bot reads them with HstUiTakePopups().
	struct Popup { int iType, iData1, iData2, iData3, iFlags; bool bOption1, bOption2; };
	virtual void AddPopup(const CvPopupInfo& kPopup)
	{
		HstUserInterface2Gen::AddPopup(kPopup);
		if(!Live() || m_akPopups.size() > 256)
			return;
		Popup k = { (int)kPopup.eButtonPopupType, kPopup.iData1, kPopup.iData2, kPopup.iData3, kPopup.iFlags, kPopup.bOption1, kPopup.bOption2 };
		HstDetHeapPauseScope kPause;	// host bookkeeping inside a DLL call: keep the deterministic layout
		m_akPopups.push_back(k);
	}
	void TakePopups(std::vector<Popup>& akOut) { akOut.clear(); akOut.swap(m_akPopups); }

protected:
	ICvGameContext1* m_pkCtx;
	HstEngineUtility* m_pkEngine;
	bool m_bCanEndTurn;
	bool m_bHasUnit;
	bool m_bHasCity;
	IDInfo m_kUnit;
	IDInfo m_kCity;
	std::vector<Popup> m_akPopups;
	bool m_bHasScratchDeal;
	unsigned int m_uiScratchDeal;
};

#endif // HSTUSERINTERFACE_H
