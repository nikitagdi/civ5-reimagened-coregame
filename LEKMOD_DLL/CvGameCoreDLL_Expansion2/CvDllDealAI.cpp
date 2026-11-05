/*	-------------------------------------------------------------------------------------------------------
	© 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */

#include "CvGameCoreDLLPCH.h"
#include "CvDllDealAI.h"
#include "CvDllContext.h"
#include "CvDllDeal.h"
#include "CvDllPlayer.h"
#include "CvDealAI.h"

#ifdef AUI_WARNING_FIXES
CvDllDealAI::CvDllDealAI(_In_ CvDealAI* pDealAI)
#else
CvDllDealAI::CvDllDealAI(CvDealAI* pDealAI)
#endif
	: m_pDealAI(pDealAI)
	, m_uiRefCount(1)
{
}
//------------------------------------------------------------------------------
CvDllDealAI::~CvDllDealAI()
{
}
//------------------------------------------------------------------------------
void* CvDllDealAI::QueryInterface(GUID guidInterface)
{
	if(guidInterface == ICvUnknown::GetInterfaceId() ||
	        guidInterface == ICvDealAI1::GetInterfaceId())
	{
		IncrementReference();
		return this;
	}

	return NULL;
}
//------------------------------------------------------------------------------
unsigned int CvDllDealAI::IncrementReference()
{
	++m_uiRefCount;
	return m_uiRefCount;
}
//------------------------------------------------------------------------------
unsigned int CvDllDealAI::DecrementReference()
{
	if(m_uiRefCount == 1)
	{
		delete this;
		return 0;
	}
	else
	{
		--m_uiRefCount;
		return m_uiRefCount;
	}
}
//------------------------------------------------------------------------------
unsigned int CvDllDealAI::GetReferenceCount()
{
	return m_uiRefCount;
}
//------------------------------------------------------------------------------
void CvDllDealAI::Destroy()
{
	DecrementReference();
}
//------------------------------------------------------------------------------
void CvDllDealAI::operator delete(void* p)
{
	CvDllGameContext::Free(p);
}
//------------------------------------------------------------------------------
void* CvDllDealAI::operator new(size_t bytes)
{
	return CvDllGameContext::Allocate(bytes);
}
//------------------------------------------------------------------------------
CvDealAI* CvDllDealAI::GetInstance()
{
	return m_pDealAI;
}
//------------------------------------------------------------------------------
ICvPlayer1* CvDllDealAI::GetPlayer()
{
	CvPlayerAI* pkPlayer = (CvPlayerAI*)m_pDealAI->GetPlayer();
	return (NULL != pkPlayer)? new CvDllPlayer(pkPlayer) : NULL;
}
//------------------------------------------------------------------------------
int CvDllDealAI::DoHumanOfferDealToThisAI(ICvDeal1* pDeal)
{
	CvDeal* pkDeal = (NULL != pDeal)? static_cast<CvDllDeal*>(pDeal)->GetInstance() : NULL;
#if defined(LEKMOD_HEADLESS_FIX_0030)
	// FIX-0030 / DS-0037: the exe calls this from the trade screen (UI.DoProposeDeal) on the proposing human's machine only;
	// the deal valuation reaches calculateGoldRate, whose cost refresh would store the treasury's saved unit costs there only
	HEADLESS_UI_TREASURY_SCOPE();
#endif
	return m_pDealAI->DoHumanOfferDealToThisAI(pkDeal);
}
//------------------------------------------------------------------------------
void CvDllDealAI::DoAcceptedDeal(PlayerTypes eFromPlayer, ICvDeal1* pDeal, int iDealValueToMe, int iValueImOffering, int iValueTheyreOffering)
{
#if defined(LEKMOD_HEADLESS_FIX_0017)
	HEADLESS_FPU_GUARD();	// FIX-0017 / DS-0028
#endif
	CvDeal* pkDeal = (NULL != pDeal)? static_cast<CvDllDeal*>(pDeal)->GetInstance() : NULL;
	if(pkDeal != NULL)
		m_pDealAI->DoAcceptedDeal(eFromPlayer, *pkDeal, iDealValueToMe, iValueImOffering, iValueTheyreOffering);
}
//------------------------------------------------------------------------------
int CvDllDealAI::DoHumanDemand(ICvDeal1* pDeal)
{
	CvDeal* pkDeal = (NULL != pDeal)? static_cast<CvDllDeal*>(pDeal)->GetInstance() : NULL;
#if defined(LEKMOD_HEADLESS_FIX_0030)
	HEADLESS_UI_TREASURY_SCOPE();	// FIX-0030 / DS-0037: UI.DoDemand, demanding human's machine only (CalculateBaseNetGold)
#endif
	return m_pDealAI->DoHumanDemand(pkDeal);
}
//------------------------------------------------------------------------------
void CvDllDealAI::DoAcceptedDemand(PlayerTypes eFromPlayer, ICvDeal1* pDeal)
{
#if defined(LEKMOD_HEADLESS_FIX_0017)
	HEADLESS_FPU_GUARD();	// FIX-0017 / DS-0028
#endif
	CvDeal* pkDeal = (NULL != pDeal)? static_cast<CvDllDeal*>(pDeal)->GetInstance() : NULL;
	if(pkDeal != NULL)
		m_pDealAI->DoAcceptedDemand(eFromPlayer, *pkDeal);
}
//------------------------------------------------------------------------------
bool CvDllDealAI::DoEqualizeDealWithHuman(ICvDeal1* pDeal, PlayerTypes eOtherPlayer, bool bDontChangeMyExistingItems, bool bDontChangeTheirExistingItems, bool& bDealGoodToBeginWith, bool& bCantMatchOffer)
{
	CvDeal* pkDeal = (NULL != pDeal)? static_cast<CvDllDeal*>(pDeal)->GetInstance() : NULL;
#if defined(LEKMOD_HEADLESS_FIX_0030)
	HEADLESS_UI_TREASURY_SCOPE();	// FIX-0030 / DS-0037: trade screen equalize / what-do-you-want buttons, local only
#endif
	return m_pDealAI->DoEqualizeDealWithHuman(pkDeal, eOtherPlayer, bDontChangeMyExistingItems, bDontChangeTheirExistingItems, bDealGoodToBeginWith, bCantMatchOffer);
}
//------------------------------------------------------------------------------