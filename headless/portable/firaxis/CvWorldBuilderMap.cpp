// CvWorldBuilderMap (WorldBuilder .Civ5Map) in memory, portable build: record defaults, scenario data, type tables, type groups and mod
// data. Written against the behaviour of the shipped game's own WorldBuilder map code (Civ5XP 1.0.3.279).
// Loading the file: CvWorldBuilderMapLoad.cpp. Save is not supported (logged).
#include "CvGameCoreDLLPCH.h"

#define CV_WORLD_BUILDER_MAP_IN_GAME	// the layout the gamecore sees (no editor events)
#include "CvWorldBuilderMap.h"
#include "CvWorldBuilderMapTypeDesc.h"
#include "lekmod_portable/host_log.h"

// ---- record defaults -------------------------------------------------
CvWorldBuilderMap::MapDescription::MapDescription() : uiWidth(0), uiHeight(0) {}

CvWorldBuilderMap::GameDescription::GameDescription()
	: uiStartingTurn(0), uiMaxTurns(0), uiTargetScore(0), iStartYear(-4000), byPlayerCount(0), byCityStateCount(0), byTeamCount(0)
{
	szDefaultSpeed[0] = 0;
}

CvWorldBuilderMap::PlotMapData::PlotMapData()
	: m_byTerrainType(0), m_byResourceType(InvalidResource), m_byFeatureType(InvalidFeature), m_byFlags(0), m_byPlotHeight(FLAT_TERRAIN),
	  m_byContinentType(0), m_byNaturalWonderType(InvalidNaturalWonder), m_byResourceAmount(0)
{
}

CvWorldBuilderMap::OldUnit::OldUnit()
	: m_wExperience(0), m_fHealth(1.0f), m_byUnitType(InvalidUnitType), m_byOwner(0xff), m_byDirection(0), m_bFortified(false), m_bEmbarked(false)
{
}

CvWorldBuilderMap::UnitV2::UnitV2()
	: m_uiExperience(0), m_uiHealth(MaxHealth), m_byUnitType(InvalidUnitType), m_byOwner(0xff), m_byDirection(0), m_byFlags(0)
{
}

CvWorldBuilderMap::Unit::Unit()
	: m_uiExperience(0), m_uiHealth(MaxHealth), m_byUnitType(InvalidUnitType), m_byOwner(0xff), m_byDirection(0), m_byFlags(0)
{
}

CvWorldBuilderMap::OldCity::OldCity() : m_uiPopulation(1), m_uiCulture(0), m_byOwner(0xff), m_bLocalizedName(false) { m_szName[0] = 0; }
CvWorldBuilderMap::CityV2::CityV2() : m_byOwner(0xff), m_byFlags(0), m_wPopulation(1), m_uiHealth(MaxHealth) { m_szName[0] = 0; }
CvWorldBuilderMap::City::City() : m_byOwner(0xff), m_byFlags(0), m_wPopulation(1), m_uiHealth(MaxHealth) { m_szName[0] = 0; }

CvWorldBuilderMap::PlotScenarioData::PlotScenarioData()
	: m_byCulture(InvalidCulture), m_byImprovement(InvalidImprovement), m_byRoute(InvalidRoute), m_byRouteOwner(0xff)
{
}

CvWorldBuilderMap::Team::Team() { m_szName[0] = 0; }

CvWorldBuilderMap::OldPlayer::OldPlayer()
	: m_uiCulture(0), m_uiGold(0), m_uiStartX(uint(-1)), m_uiStartY(uint(-1)), m_byTeam(InvalidTeam), m_bPlayable(true)
{
	m_szCivType[0] = m_szTeamColor[0] = m_szEra[0] = m_szHandicap[0] = 0;
}

CvWorldBuilderMap::Player::Player()
	: m_uiCulture(0), m_uiGold(0), m_uiStartX(uint(-1)), m_uiStartY(uint(-1)), m_byTeam(InvalidTeam), m_bPlayable(true)
{
	m_szLeaderName[0] = m_szCivName[0] = m_szCivType[0] = m_szTeamColor[0] = m_szEra[0] = m_szHandicap[0] = 0;
}

CvWorldBuilderMap::PlotMapData CvWorldBuilderMap::sm_kErrorPlotMapData;
CvWorldBuilderMap::PlotScenarioData CvWorldBuilderMap::sm_kErrorPlotScenarioData;
CvWorldBuilderMap::Player CvWorldBuilderMap::sm_kErrorPlayer;
CvWorldBuilderMap::Team CvWorldBuilderMap::sm_kErrorTeam;

// ---- CvWorldBuilderMap -----------------------------------------------------------------------------------
CvWorldBuilderMap::CvWorldBuilderMap() : m_uiFlags(FLAG_WORLD_WRAP | FLAG_TRANSFERABLE)
{
	ZeroMemory(m_aiCityStateInfluence, sizeof(m_aiCityStateInfluence));
	ZeroMemory(m_szWorldType, sizeof(m_szWorldType));
}

CvWorldBuilderMap::~CvWorldBuilderMap() {}

void CvWorldBuilderMap::Resize(uint uiWidth, uint uiHeight)
{
	if (uiHeight * uiWidth <= MaxMapSize && uiWidth != 0 && uiHeight != 0)
	{
		m_kMapDesc.uiWidth = uiWidth;
		m_kMapDesc.uiHeight = uiHeight;
	}
}

bool CvWorldBuilderMap::Save(const wchar_t*, const CvWorldBuilderMapTypeDesc&, bool) const
{
	lekmod_portable::LogStubOnce("CvWorldBuilderMap::Save (writing WorldBuilder maps is not supported)");
	return false;
}

void CvWorldBuilderMap::ClearScenarioData(bool bPreserveImprovements)
{
	const uint uiPlotCount = m_kMapDesc.uiWidth * m_kMapDesc.uiHeight;
	if (bPreserveImprovements)
	{
		for (uint i = 0; i < uiPlotCount && i < MaxMapSize; ++i)
		{
			PlotScenarioData& kPlot = m_akScenarioData[i];
			const byte byImprovement = kPlot.m_byImprovement;
			kPlot = PlotScenarioData();
			kPlot.m_byImprovement = byImprovement;
		}
	}
	else if (uiPlotCount != 0)
	{
		memset(m_akScenarioData, 0xff, (uiPlotCount < MaxMapSize ? uiPlotCount : MaxMapSize) * sizeof(PlotScenarioData));
	}

	m_kGameDesc.uiStartingTurn = 0;
	m_kGameDesc.uiMaxTurns = 0;
	m_kGameDesc.uiTargetScore = 0;
	m_kGameDesc.iStartYear = -4000;
	m_kGameDesc.byPlayerCount = 0;
	m_kGameDesc.byCityStateCount = 0;
	m_kGameDesc.byTeamCount = 0;
	m_kGameDesc.szDefaultSpeed[0] = 0;

	m_kUnits.Release();
	m_kUnitNames.Release();
	m_kCities.Release();
	m_kVictoryTypes.Clear();
	m_kGameOptions.Clear();
	m_kVisibility.Reset();
	ZeroMemory(m_aiCityStateInfluence, sizeof(m_aiCityStateInfluence));
}

void CvWorldBuilderMap::SetWorldType(const char* szWorldType)
{
	if (szWorldType && *szWorldType && strlen(szWorldType) < sizeof(m_szWorldType))
	{
		strcpy_s(m_szWorldType, sizeof(m_szWorldType), szWorldType);
		return;
	}
	ZeroMemory(m_szWorldType, sizeof(m_szWorldType));
}

namespace {
void SwapOwner(byte& byOwner, byte byFirst, byte bySecond)
{
	if (byOwner == byFirst)
		byOwner = bySecond;
	else if (byOwner == bySecond)
		byOwner = byFirst;
}
}  // namespace

// every owner reference on the map follows the swap.
void CvWorldBuilderMap::SwapPlayers(byte byPlayer1, byte byPlayer2)
{
	if (byPlayer1 >= m_kGameDesc.byPlayerCount || byPlayer2 >= m_kGameDesc.byPlayerCount || byPlayer1 == byPlayer2) return;

	const Player kTemp = m_akPlayers[byPlayer1];
	m_akPlayers[byPlayer1] = m_akPlayers[byPlayer2];
	m_akPlayers[byPlayer2] = kTemp;

	const uint uiPlotCount = m_kMapDesc.uiWidth * m_kMapDesc.uiHeight;
	for (uint uiPlot = 0; uiPlot < uiPlotCount; ++uiPlot)
	{
		PlotScenarioData& kPlot = GetPlotScenarioData(uiPlot);
		SwapOwner(kPlot.m_byCulture, byPlayer1, byPlayer2);
		SwapOwner(kPlot.m_byRouteOwner, byPlayer1, byPlayer2);
		City* pkCity = m_kCities[kPlot.m_hCity];
		if (pkCity != NULL) SwapOwner(pkCity->m_byOwner, byPlayer1, byPlayer2);
		for (Unit* pkUnit = m_kUnits[kPlot.m_hUnitStack]; pkUnit != NULL; pkUnit = m_kUnits[pkUnit->m_hStackedUnit])
			SwapOwner(pkUnit->m_byOwner, byPlayer1, byPlayer2);
	}

	for (uint i = 0; i < m_kGameDesc.byCityStateCount; ++i)
		std::swap(m_aiCityStateInfluence[byPlayer1][i], m_aiCityStateInfluence[byPlayer2][i]);
}

// ---- type descriptions  ------------------------------------------------------------
CvWorldBuilderMapTypeDesc::CvWorldBuilderMapTypeDesc()
	: m_kTerrainTypes(0x20), m_kFeatures(0x80), m_kNaturalWonders(0x80), m_kResources(0x40), m_kImprovements(0x40), m_kUnits(0x100),
	  m_kBuildingTypes(0x100), m_kTechs(0x100), m_kPolicies(0x100), m_kUnitPromotions(0x100)
{
}

uint CvWorldBuilderMapTypeDesc::TypeMap::Add(const char* szType) { return Add(String(szType)); }
uint CvWorldBuilderMapTypeDesc::TypeMap::Add(const String& szType)
{
	asTypes.push_back(szType);
	return (uint)asTypes.size() - 1;
}
const CvWorldBuilderMapTypeDesc::String& CvWorldBuilderMapTypeDesc::TypeMap::Get(uint uiIndex) const
{
	static String s_empty;
	return uiIndex < asTypes.size() ? asTypes[uiIndex] : s_empty;
}
bool CvWorldBuilderMapTypeDesc::TypeMap::FindType(const char* szType, uint& uiTypeOut) const
{
	for (uint i = 0; i < asTypes.size(); ++i)
	{
		const char* sz = asTypes[i];
		if (sz && szType && strcmp(sz, szType) == 0)
		{
			uiTypeOut = i;
			return true;
		}
	}
	return false;
}

// Serialized: the names one after another, each NUL-terminated (a NULL name as an empty one).
const size_t CvWorldBuilderMapTypeDesc::TypeMap::GetSerializedSize() const
{
	size_t n = 0;
	for (uint i = 0; i < asTypes.size(); ++i)
	{
		const char* sz = asTypes[i];
		n += (sz ? strlen(sz) : 0) + 1;
	}
	return n;
}
void CvWorldBuilderMapTypeDesc::TypeMap::Serialize(void* pvBuffer) const
{
	char* p = static_cast<char*>(pvBuffer);
	for (uint i = 0; i < asTypes.size(); ++i)
	{
		const char* sz = asTypes[i];
		const size_t n = sz ? strlen(sz) : 0;
		if (n) memcpy(p, sz, n);
		p[n] = 0;
		p += n + 1;
	}
}
// The buffer's last byte is forced to NUL.
void CvWorldBuilderMapTypeDesc::TypeMap::Deserialize(void* pvBuffer, size_t stSize)
{
	asTypes.clear();
	if (!pvBuffer || stSize == 0) return;
	char* p = static_cast<char*>(pvBuffer);
	const char* end = p + stSize;
	p[stSize - 1] = 0;
	while (p < end)
	{
		asTypes.push_back(String(p));
		p += strlen(p) + 1;
	}
}

// The string is interned for the process lifetime (type names are few and permanent).
CvWorldBuilderMapTypeDesc::String::String(const char* sz) : m_sz(NULL) { *this = sz; }
const char* CvWorldBuilderMapTypeDesc::String::operator=(const char* sz)
{
	static std::set<std::string>* s_pool = new std::set<std::string>();
	m_sz = sz ? s_pool->insert(sz).first->c_str() : NULL;
	return m_sz;
}

// ---- type groups ---------------------------------------------------------
// The buffer holds entries [length byte][name][NUL]; the next entry follows the NUL.
CvWorldBuilderMapTypeGroup::CvWorldBuilderMapTypeGroup() : m_szBuffer(NULL), m_uiBufferSize(0) {}
CvWorldBuilderMapTypeGroup::CvWorldBuilderMapTypeGroup(const CvWorldBuilderMapTypeGroup& kRhs) : m_szBuffer(NULL), m_uiBufferSize(0) { *this = kRhs; }
CvWorldBuilderMapTypeGroup::~CvWorldBuilderMapTypeGroup() { Clear(); }
const CvWorldBuilderMapTypeGroup& CvWorldBuilderMapTypeGroup::operator=(const CvWorldBuilderMapTypeGroup& kRhs)
{
	if (this != &kRhs) Deserialize(kRhs.m_szBuffer, kRhs.m_uiBufferSize);
	return *this;
}
bool CvWorldBuilderMapTypeGroup::Contains(const char* szType) const
{
	if (szType == NULL || *szType == '\0' || m_szBuffer == NULL || m_uiBufferSize == 0) return false;
	uint uiOffset = 0;
	do
	{
		// A name without its NUL inside the buffer (a damaged file) ends the walk.
		const char* szName = m_szBuffer + uiOffset + 1;
		if (uiOffset + 1 >= m_uiBufferSize || !memchr(szName, 0, m_uiBufferSize - uiOffset - 1)) return false;
		if (strcmp(szName, szType) == 0) return true;
		uiOffset += 2 + (uint)(int)(signed char)m_szBuffer[uiOffset];
	} while (uiOffset < m_uiBufferSize);
	return false;
}
void CvWorldBuilderMapTypeGroup::Add(const char* szType)
{
	if (szType == NULL || *szType == '\0') return;
	const size_t n = strlen(szType);
	if (n > 0xff || Contains(szType)) return;
	char* p = new char[m_uiBufferSize + n + 2];
	if (m_uiBufferSize) memcpy(p, m_szBuffer, m_uiBufferSize);
	p[m_uiBufferSize] = (char)n;
	memcpy(p + m_uiBufferSize + 1, szType, n + 1);
	delete[] m_szBuffer;
	m_szBuffer = p;
	m_uiBufferSize += (uint)n + 2;
}
void CvWorldBuilderMapTypeGroup::Remove(const char* szType)
{
	if (szType == NULL || *szType == '\0' || !Contains(szType)) return;
	CvWorldBuilderMapTypeGroup kept;
	for (uint uiOffset = 0; uiOffset + 1 < m_uiBufferSize; uiOffset += 2 + (uint)(int)(signed char)m_szBuffer[uiOffset])
		if (strcmp(m_szBuffer + uiOffset + 1, szType) != 0) kept.Add(m_szBuffer + uiOffset + 1);
	*this = kept;
}
void CvWorldBuilderMapTypeGroup::Clear()
{
	delete[] m_szBuffer;
	m_szBuffer = NULL;
	m_uiBufferSize = 0;
}
void CvWorldBuilderMapTypeGroup::Serialize(void* p) const
{
	if (m_uiBufferSize) memcpy(p, m_szBuffer, m_uiBufferSize);
}
void CvWorldBuilderMapTypeGroup::Deserialize(const void* p, uint uiSize)
{
	Clear();
	if (!p || !uiSize) return;
	m_szBuffer = new char[uiSize];
	memcpy(m_szBuffer, p, uiSize);
	m_uiBufferSize = uiSize;
}

// ---- mod data -------------------------------------------------------------
// Serialized CvWorldBuilderMapModData: map ModType, plot ModType, [u32 n][n dwords: map fields, then plot fields per plot],
// string buffer [u32 size][size bytes]. A ModType: [u32 fields][u32 enums][fields * {type, name id, default}][per enum: u32 n, n ids].
namespace {
inline uint ModU32(const byte* p) { uint v; memcpy(&v, p, 4); return v; }
}  // namespace

ModDataStringBuffer::ModDataStringBuffer() : m_uiSize(0), m_uiReservedSize(0), m_szBuffer(NULL) {}
ModDataStringBuffer::~ModDataStringBuffer() { Clear(); }
void ModDataStringBuffer::Clear()
{
	delete[] m_szBuffer;
	m_szBuffer = NULL;
	m_uiSize = m_uiReservedSize = 0;
}
uint ModDataStringBuffer::AddString(const char* sz)
{
	if (!sz) sz = "";
	uint n = (uint)strlen(sz) + 1;
	if (m_uiSize + n > m_uiReservedSize)
	{
		uint cap = (m_uiSize + n) * 2;
		char* p = new char[cap];
		if (m_uiSize) memcpy(p, m_szBuffer, m_uiSize);
		delete[] m_szBuffer;
		m_szBuffer = p;
		m_uiReservedSize = cap;
	}
	memcpy(m_szBuffer + m_uiSize, sz, n);
	uint id = m_uiSize;
	m_uiSize += n;
	return id;
}
const char* ModDataStringBuffer::GetString(uint uiStringID) const { return uiStringID < m_uiSize ? m_szBuffer + uiStringID : ""; }
size_t ModDataStringBuffer::GetSerializedSize() const { return sizeof(uint) + m_uiSize; }
void ModDataStringBuffer::Serialize(void* pv) const
{
	memcpy(pv, &m_uiSize, sizeof(uint));
	if (m_uiSize) memcpy(static_cast<byte*>(pv) + sizeof(uint), m_szBuffer, m_uiSize);
}
void ModDataStringBuffer::Deserialize(const void* pv)
{
	Clear();
	const byte* p = static_cast<const byte*>(pv);
	const uint uiSize = ModU32(p);
	if (uiSize == 0) return;
	m_szBuffer = new char[uiSize + 1];
	memcpy(m_szBuffer, p + sizeof(uint), uiSize);
	m_szBuffer[uiSize] = 0;  // GetString stays inside the buffer
	m_uiSize = m_uiReservedSize = uiSize;
}

ModEnum::ModEnum() : m_pkStringBuffer(NULL) {}
ModEnum::ModEnum(const ModEnum& kRhs) : m_pkStringBuffer(kRhs.m_pkStringBuffer), m_kMembers(kRhs.m_kMembers) {}
ModEnum::ModEnum(ModDataStringBuffer& kStringBuffer) : m_pkStringBuffer(&kStringBuffer) {}
void ModEnum::AddMember(const char* szMember)
{
	if (m_pkStringBuffer) m_kMembers.push_back(m_pkStringBuffer->AddString(szMember));
}
uint ModEnum::GetMemberCount() const { return m_kMembers.size(); }
const char* ModEnum::GetMember(uint uiMember) const
{
	return (m_pkStringBuffer && uiMember < m_kMembers.size()) ? m_pkStringBuffer->GetString(m_kMembers[uiMember]) : NULL;
}
size_t ModEnum::GetSerializedSize() const { return sizeof(uint) + m_kMembers.size() * sizeof(uint); }
void ModEnum::Deserialize(const void* pv)
{
	const byte* p = static_cast<const byte*>(pv);
	const uint n = ModU32(p);
	m_kMembers.clear();
	for (uint i = 0; i < n; ++i) m_kMembers.push_back(ModU32(p + sizeof(uint) * (1 + i)));
}

ModType::ModType(ModDataStringBuffer& kStringBuffer) : m_kStringBuffer(kStringBuffer) {}
void ModType::Clear()
{
	m_kFields.clear();
	m_kEnums.clear();
}
uint ModType::GetFieldCount() const { return m_kFields.size(); }
const char* ModType::GetFieldName(uint uiField) const { return uiField < m_kFields.size() ? m_kStringBuffer.GetString(m_kFields[uiField].dwFieldName) : NULL; }
ModType::FieldType ModType::GetFieldType(uint uiField) const
{
	if (uiField >= m_kFields.size()) return TYPE_INVALID;
	dword t = m_kFields[uiField].dwFieldType;
	return t >= (dword)TYPE_ENUM ? TYPE_ENUM : (FieldType)t;
}
bool ModType::GetFieldDefaultAsBool(uint uiField) const { return uiField < m_kFields.size() && m_kFields[uiField].dwDefault != 0; }
int ModType::GetFieldDefaultAsInt(uint uiField) const { return uiField < m_kFields.size() ? (int)m_kFields[uiField].dwDefault : 0; }
float ModType::GetFieldDefaultAsFloat(uint uiField) const
{
	if (uiField >= m_kFields.size()) return 0.0f;
	float f;
	memcpy(&f, &m_kFields[uiField].dwDefault, sizeof(f));
	return f;
}
const char* ModType::GetFieldDefaultAsString(uint uiField) const { return uiField < m_kFields.size() ? m_kStringBuffer.GetString(m_kFields[uiField].dwDefault) : ""; }
const ModEnum* ModType::GetFieldEnumType(uint uiField) const
{
	if (uiField >= m_kFields.size() || m_kFields[uiField].dwFieldType < (dword)TYPE_ENUM) return NULL;
	uint e = m_kFields[uiField].dwFieldType - (dword)TYPE_ENUM;
	return e < m_kEnums.size() ? &m_kEnums[e] : NULL;
}
size_t ModType::GetSerializedSize() const
{
	size_t n = 2 * sizeof(uint) + m_kFields.size() * sizeof(Field);
	for (uint i = 0; i < m_kEnums.size(); ++i) n += m_kEnums[i].GetSerializedSize();
	return n;
}
void ModType::Deserialize(const void* pv)
{
	const byte* p = static_cast<const byte*>(pv);
	const uint uiFields = ModU32(p), uiEnums = ModU32(p + sizeof(uint));
	Clear();
	p += 2 * sizeof(uint);
	for (uint i = 0; i < uiFields; ++i, p += sizeof(Field))
	{
		Field f;
		f.dwFieldType = ModU32(p);
		f.dwFieldName = ModU32(p + 4);
		f.dwDefault = ModU32(p + 8);
		m_kFields.push_back(f);
	}
	for (uint i = 0; i < uiEnums; ++i)
	{
		m_kEnums.push_back(ModEnum(m_kStringBuffer));
		m_kEnums[i].Deserialize(p);
		p += m_kEnums[i].GetSerializedSize();
	}
}

ModDataEntry::ModDataEntry(ModType& kModType, dword* aFields) : m_kModType(kModType), m_aFields(aFields) {}
bool ModDataEntry::GetFieldAsBool(uint uiField) const { return m_aFields ? m_aFields[uiField] != 0 : m_kModType.GetFieldDefaultAsBool(uiField); }
int ModDataEntry::GetFieldAsInt(uint uiField) const { return m_aFields ? (int)m_aFields[uiField] : m_kModType.GetFieldDefaultAsInt(uiField); }
float ModDataEntry::GetFieldAsFloat(uint uiField) const
{
	if (!m_aFields) return m_kModType.GetFieldDefaultAsFloat(uiField);
	float f;
	memcpy(&f, &m_aFields[uiField], sizeof(f));
	return f;
}
const char* ModDataEntry::GetFieldAsString(uint uiField) const
{
	return m_aFields ? m_kModType.m_kStringBuffer.GetString(m_aFields[uiField]) : m_kModType.GetFieldDefaultAsString(uiField);
}
bool ModDataEntry::IsFieldDefaultValue(uint uiField) const
{
	return !m_aFields || uiField >= m_kModType.GetFieldCount() || m_aFields[uiField] == m_kModType.m_kFields[uiField].dwDefault;
}

CvWorldBuilderMapModData::CvWorldBuilderMapModData() : m_kMapModData(m_kStringBuffer), m_kPlotModData(m_kStringBuffer) {}

void CvWorldBuilderMapModData::InitDataEntries(uint uiPlotCount)
{
	m_kData.resize(m_kMapModData.GetFieldCount() + m_kPlotModData.GetFieldCount() * uiPlotCount);
}

void CvWorldBuilderMapModData::ResetTypes()
{
	m_kMapModData.Clear();
	m_kPlotModData.Clear();
	m_kStringBuffer.Clear();
	m_kData.clear();
}

size_t CvWorldBuilderMapModData::GetSerializedSize() const
{
	return m_kMapModData.GetSerializedSize() + m_kPlotModData.GetSerializedSize() + sizeof(uint) + m_kData.size() * sizeof(dword) +
		m_kStringBuffer.GetSerializedSize();
}

void CvWorldBuilderMapModData::Deserialize(const void* pv)
{
	const byte* p = static_cast<const byte*>(pv);
	m_kMapModData.Deserialize(p);
	p += m_kMapModData.GetSerializedSize();
	m_kPlotModData.Deserialize(p);
	p += m_kPlotModData.GetSerializedSize();
	const uint n = ModU32(p);
	p += sizeof(uint);
	m_kData.clear();
	for (uint i = 0; i < n; ++i) m_kData.push_back(ModU32(p + i * sizeof(dword)));
	p += n * sizeof(dword);
	m_kStringBuffer.Deserialize(p);
}

// Without stored values (no mod data in the map, or fewer values than fields) an entry reports the type defaults.
ModDataEntry CvWorldBuilderMapModData::GetMapDataEntry()
{
	const uint uiFields = m_kMapModData.GetFieldCount();
	return ModDataEntry(m_kMapModData, uiFields && m_kData.size() >= uiFields ? &m_kData[0] : NULL);
}
ModDataEntry CvWorldBuilderMapModData::GetPlotDataEntry(uint uiPlot)
{
	const uint uiPlotFields = m_kPlotModData.GetFieldCount(), uiMapFields = m_kMapModData.GetFieldCount();
	const size_t first = (size_t)uiPlotFields * uiPlot + uiMapFields;
	return ModDataEntry(m_kPlotModData, uiPlotFields && m_kData.size() >= first + uiPlotFields ? &m_kData[(uint)first] : NULL);
}
