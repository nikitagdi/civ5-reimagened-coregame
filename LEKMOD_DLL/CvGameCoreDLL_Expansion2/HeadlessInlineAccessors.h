#pragma once
// OPT-0061 (LEKMOD_HEADLESS_OPT_INLINE_ACCESSORS): inline definitions of the hottest trivial accessors.
//
// These member functions are declared in their class headers and were defined out of line in their .cpp files. Without
// /GL (forbidden, I8) the compiler can never inline a call into another translation unit, so every call paid call/ret,
// argument pushes and the nested out-of-line calls (CvGlobals::getBuildingInfo -> getNumBuildingInfos ->
// CvBuildingXMLEntries::GetNumBuildings, GetBuildingEntries). The bodies below are the golden bodies verbatim (same
// tests, same order, same NULL / -1 fallbacks); the .cpp definitions are compiled only without the flag. Included at the
// end of CvGameCoreDLLPCH.h, which every translation unit of the DLL includes first, so every caller sees the definition.
//
// iteration order: no loops. RNG: none. float: none (every function returns an int, enum, bool, pointer or reference and
// reads no floating-point data), so x87 code in the callers computes the same values.
// Signatures: the non-AUI_WARNING_FIXES / non-AUI_CONSTIFY variants (the ones golden compiles); the header refuses others.

#if defined(LEKMOD_HEADLESS_OPT_INLINE_ACCESSORS)
#if defined(AUI_WARNING_FIXES) || defined(AUI_CONSTIFY)
#error "OPT-0061 inline accessors are written for the golden signatures (AUI_WARNING_FIXES and AUI_CONSTIFY off)"
#endif

// ---- database entry tables ---------------------------------------------------------------------------------------
__forceinline std::vector<CvBuildingEntry*>& CvBuildingXMLEntries::GetBuildingEntries()
{
	return m_paBuildingEntries;
}
__forceinline int CvBuildingXMLEntries::GetNumBuildings()
{
	return m_paBuildingEntries.size();
}
__forceinline std::vector<CvImprovementEntry*>& CvImprovementXMLEntries::GetImprovementEntries()
{
	return m_paImprovementEntries;
}
__forceinline int CvImprovementXMLEntries::GetNumImprovements()
{
	return m_paImprovementEntries.size();
}
__forceinline std::vector<CvUnitEntry*>& CvUnitXMLEntries::GetUnitEntries()
{
	return m_paUnitEntries;
}
__forceinline int CvUnitXMLEntries::GetNumUnits()
{
	return m_paUnitEntries.size();
}
__forceinline std::vector<CvPromotionEntry*>& CvPromotionXMLEntries::GetPromotionEntries()
{
	return m_paPromotionEntries;
}

// ---- CvGlobals info getters --------------------------------------------------------------------------------------
__forceinline int CvGlobals::getNumBuildingInfos()
{
	return m_pBuildings->GetNumBuildings();
}
__forceinline CvBuildingEntry* CvGlobals::getBuildingInfo(BuildingTypes eBuildingNum)
{
	CvAssert(eBuildingNum > -1);
	CvAssert(eBuildingNum < GC.getNumBuildingInfos());
	if(eBuildingNum > -1 && eBuildingNum < GC.getNumBuildingInfos())
		return m_pBuildings->GetBuildingEntries()[eBuildingNum];
	else
		return NULL;
}
__forceinline int CvGlobals::getNumBuildingClassInfos()
{
	return (int)m_paBuildingClassInfo.size();
}
__forceinline CvBuildingClassInfo* CvGlobals::getBuildingClassInfo(BuildingClassTypes eBuildingClassNum)
{
	CvAssert(eBuildingClassNum > -1);
	CvAssert(eBuildingClassNum < GC.getNumBuildingClassInfos());
	if(eBuildingClassNum > -1 && eBuildingClassNum < (int)m_paBuildingClassInfo.size())
		return m_paBuildingClassInfo[eBuildingClassNum];
	else
		return NULL;
}
__forceinline CvCivilizationInfo* CvGlobals::getCivilizationInfo(CivilizationTypes eCivilizationNum)
{
	CvAssert(eCivilizationNum > -1);
	CvAssert(eCivilizationNum < GC.getNumCivilizationInfos());
	if(eCivilizationNum > -1 && eCivilizationNum < (int)m_paCivilizationInfo.size())
		return m_paCivilizationInfo[eCivilizationNum];
	else
		return NULL;
}
__forceinline int CvGlobals::getNumImprovementInfos()
{
	return m_pImprovements->GetNumImprovements();
}
__forceinline CvImprovementEntry* CvGlobals::getImprovementInfo(ImprovementTypes eImprovementNum)
{
	CvAssert(eImprovementNum > -1);
	CvAssert(eImprovementNum < GC.getNumImprovementInfos());
	if(eImprovementNum > -1 && eImprovementNum < GC.getNumImprovementInfos())
		return m_pImprovements->GetImprovementEntries()[eImprovementNum];
	else
		return NULL;
}
__forceinline CvTerrainInfo* CvGlobals::getTerrainInfo(TerrainTypes eTerrainNum)
{
	CvAssert(eTerrainNum > -1);
	CvAssert(eTerrainNum < GC.getNumTerrainInfos());
	if(eTerrainNum > -1 && eTerrainNum < (int)m_paTerrainInfo.size())
		return m_paTerrainInfo[eTerrainNum];
	else
		return NULL;
}
__forceinline CvResourceInfo* CvGlobals::getResourceInfo(ResourceTypes eResourceNum)
{
	CvAssert(eResourceNum > -1);
	CvAssert(eResourceNum < GC.getNumResourceInfos());
	if(eResourceNum > -1 && eResourceNum < (int)m_paResourceInfo.size())
		return m_paResourceInfo[eResourceNum];
	else
		return NULL;
}
__forceinline CvFeatureInfo* CvGlobals::getFeatureInfo(FeatureTypes eFeatureNum)
{
	CvAssert(eFeatureNum > -1);
	CvAssert(eFeatureNum < GC.getNumFeatureInfos());
	if(eFeatureNum > -1 && eFeatureNum < (int)m_paFeatureInfo.size())
		return m_paFeatureInfo[eFeatureNum];
	else
		return NULL;
}
__forceinline int CvGlobals::getNumUnitInfos()
{
	return m_pUnits->GetNumUnits();
}
__forceinline CvUnitEntry* CvGlobals::getUnitInfo(UnitTypes eUnitNum)
{
	CvAssert(eUnitNum > -1);
	CvAssert(eUnitNum < GC.getNumUnitInfos());
	if(eUnitNum > -1 && eUnitNum < GC.getNumUnitInfos())
		return m_pUnits->GetUnitEntries()[eUnitNum];
	else
		return NULL;
}
__forceinline CvBuildInfo* CvGlobals::getBuildInfo(BuildTypes eBuildNum)
{
	CvAssert(eBuildNum > -1);
	CvAssert(eBuildNum < GC.getNumBuildInfos());
	if(eBuildNum > -1 && eBuildNum < (int)m_paBuildInfo.size())
		return m_paBuildInfo[eBuildNum];
	else
		return NULL;
}
__forceinline int CvGlobals::getNumPromotionInfos()
{
	return (int)m_pPromotions->GetPromotionEntries().size();
}
__forceinline CvPromotionEntry* CvGlobals::getPromotionInfo(PromotionTypes ePromotionNum)
{
	CvAssert(ePromotionNum > -1);
	CvAssert(ePromotionNum < GC.getNumPromotionInfos());
	if(ePromotionNum > -1 && ePromotionNum < GC.getNumPromotionInfos())
		return m_pPromotions->GetPromotionEntries()[ePromotionNum];
	else
		return NULL;
}

// ---- CvInfos -----------------------------------------------------------------------------------------------------
__forceinline int CvCivilizationInfo::getCivilizationBuildings(int i) const
{
	CvAssertMsg(i < GC.getNumBuildingClassInfos(), "Index out of bounds");
	CvAssertMsg(i > -1, "Index out of bounds");
	return m_piCivilizationBuildings ? m_piCivilizationBuildings[i] : -1;
}

// ---- pregame slot data -------------------------------------------------------------------------------------------
namespace CvPreGame
{
extern const std::vector<CivilizationTypes>& sr_HeadlessCivilizations;	// = s_civilizations (CvPreGame.cpp)
__forceinline CivilizationTypes civilization(PlayerTypes p)
{
	if(p >= 0 && p < MAX_PLAYERS)
		return sr_HeadlessCivilizations[p];
	return NO_CIVILIZATION;
}
}

// ---- game objects ------------------------------------------------------------------------------------------------
__forceinline CvPlayerTraits* CvPlayer::GetPlayerTraits() const
{
	return m_pTraits;
}
__forceinline CivilizationTypes CvPlayer::getCivilizationType() const
{
	return CvPreGame::civilization(GetID());
}
inline CvCivilizationInfo& CvPlayer::getCivilizationInfo() const
{
	CvCivilizationInfo* pkCivilizationInfo = GC.getCivilizationInfo(getCivilizationType());
	if(pkCivilizationInfo == NULL)
	{
		const char* szError = "ERROR: Player does not contain valid civilization type!!";
		GC.LogMessage(szError);
		CvAssertMsg(false, szError);
	}

#pragma warning ( push )
#pragma warning ( disable : 6011 ) // Dereferencing NULL pointer
	return *pkCivilizationInfo;
#pragma warning ( pop )
}
__forceinline CvPlot* CvCity::plot() const
{
	if ((m_iX != INVALID_PLOT_COORD) && (m_iY != INVALID_PLOT_COORD))
		return GC.getMap().plotUnchecked(m_iX, m_iY);
	return NULL;
}
__forceinline CvCityBuildings* CvCity::GetCityBuildings() const
{
	VALIDATE_OBJECT
	return m_pCityBuildings;
}
__forceinline TeamTypes CvUnit::getTeam() const
{
	VALIDATE_OBJECT
	return CvPlayer::getTeam( getOwner() );
}
__forceinline CvPlot* CvUnit::plot() const
{
	VALIDATE_OBJECT
	return GC.getMap().plotCheckInvalid(getX(), getY());
}
__forceinline int CvPlot::GetPlotIndex() const
{
	return GC.getMap().plotNum(getX(),getY());
}
__forceinline int CvCityBuildings::GetNumRealBuilding(BuildingTypes eIndex) const
{
	CvAssertMsg(eIndex >= 0, "eIndex expected to be >= 0");
	CvAssertMsg(eIndex < m_pBuildings->GetNumBuildings(), "eIndex expected to be < m_pBuildings->GetNumBuildings()");
	return m_paiNumRealBuilding[eIndex];
}
__forceinline int CvCityBuildings::GetNumFreeBuilding(BuildingTypes eIndex) const
{
	CvAssertMsg(eIndex >= 0, "eIndex expected to be >= 0");
	CvAssertMsg(eIndex < m_pBuildings->GetNumBuildings(), "eIndex expected to be < m_pBuildings->GetNumBuildings()");
	return m_paiNumFreeBuilding[eIndex];
}
__forceinline int CvCityBuildings::GetNumBuilding(BuildingTypes eIndex) const
{
	CvAssertMsg(eIndex != NO_BUILDING, "BuildingType eIndex is expected to not be NO_BUILDING");

	if(GC.getCITY_MAX_NUM_BUILDINGS() <= 1)
	{
		return std::max(GetNumRealBuilding(eIndex), GetNumFreeBuilding(eIndex));
	}
	else
	{
		return (GetNumRealBuilding(eIndex) + GetNumFreeBuilding(eIndex));
	}
}

// ---- second batch (profile of the first one: callers that still paid a call) ------------------------------------
__forceinline CvBuildingEntry* CvBuildingXMLEntries::GetEntry(int index)
{
	return m_paBuildingEntries[index];
}
__forceinline CvUnitEntry* CvUnitXMLEntries::GetEntry(int index)
{
	return m_paUnitEntries[index];
}
__forceinline int CvUnitEntry::GetDomainType() const
{
	return m_iDomainType;
}
__forceinline CvTechEntry* CvTechXMLEntries::GetEntry(int index)
{
	return m_paTechEntries[index];
}
__forceinline bool CvTeamTechs::HasTech(TechTypes eIndex) const
{
	if(eIndex == NO_TECH)
	{
		return true;
	}

	CvAssertMsg(eIndex >= 0, "eIndex is expected to be non-negative (invalid Index)");
	CvAssertMsg(eIndex < GC.getNumTechInfos(), "eIndex is expected to be within maximum bounds (invalid Index)");
	CvAssertMsg(m_pabHasTech != NULL, "m_pabHasTech is not expected to be equal with NULL");
	if(m_pabHasTech != NULL)
		return m_pabHasTech[eIndex];
	else
		return false;
}
__forceinline CivilizationTypes CvCity::getCivilizationType() const
{
	VALIDATE_OBJECT
	return GET_PLAYER(getOwner()).getCivilizationType();
}
inline CvCivilizationInfo& CvCity::getCivilizationInfo() const
{
	return GET_PLAYER(getOwner()).getCivilizationInfo();
}
inline DomainTypes CvUnit::getDomainType() const
{
	VALIDATE_OBJECT
	DomainTypes eDomain = (DomainTypes) m_pUnitInfo->GetDomainType();
	// antonjs: Added for Smoky Skies Scenario. If unit is DOMAIN_HOVER, dynamically determine its domain by the type of terrain it is over
	if (eDomain == DOMAIN_HOVER)
	{
		CvPlot* pPlot = plot();
		CvAssertMsg(pPlot, "pPlot is unexpectedly NULL. Please send Anton your save file and version.");
		if (pPlot)
		{
			if (pPlot->isWater())
			{
				return DOMAIN_SEA;
			}
			else
			{
				return DOMAIN_LAND;
			}
		}
	}

	return eDomain;
}
#endif
