// CvWorldBuilderMap::Load / PreviewPlayableCivCount (WorldBuilder .Civ5Map, file versions 6-12), portable build.
// Written against the behaviour of the shipped game's own loader (Civ5XP 1.0.3.279): Load, PreviewPlayableCivCount, the record
// conversions and the version loaders Load6..Load11Plus. One body (Load11Plus) serves versions 6-12;
// LoadN calls it with its version. The file is read whole through the host (lekmod_portable/wb_map.h) into a memory FIFile.
//
// LP64: the original reads records straight into its structs (i386 layout). Here every record is decoded field by field from the
// i386 offsets: on x86_64 std::bitset words are 8 bytes, so Unit (promotions at +0x14) and Player / OldCity grow or move.
#include "CvGameCoreDLLPCH.h"

#include <bitset>
#include <string>
#include <vector>

#define CV_WORLD_BUILDER_MAP_IN_GAME	// the layout the gamecore sees (no editor events)
#include "CvWorldBuilderMap.h"
#include "CvWorldBuilderMapTypeDesc.h"
#include "FireWorks/FIFile.h"
#include "lekmod_portable/host_log.h"
#include "lekmod_portable/wb_map.h"

using lekmod_portable::Log;
using lekmod_portable::LOG_WARN;
using lekmod_portable::LOG_ERROR;

// ---- host file source ------------------------------------------------------------------------------
namespace {
lekmod_portable::WbMapReadFn g_wbRead = NULL;
void* g_wbReadUser = NULL;
}  // namespace

namespace lekmod_portable {

void SetWbMapReader(WbMapReadFn fn, void* user)
{
	g_wbRead = fn;
	g_wbReadUser = user;
}

std::string WbMapPath(const wchar_t* path)
{
	std::string out;
	if (!path) return out;
	for (const wchar_t* p = path; *p; ++p)
	{
		const unsigned int c = (unsigned int)*p;
		if (c == '\\') out += '/';
		else if (c < 0x80) out += (char)c;
		else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
		else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
		else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
	}
	return out;
}

bool ReadWbMapFile(const wchar_t* path, std::string& bytes)
{
	bytes.clear();
	const std::string p = WbMapPath(path);
	if (p.empty() || !g_wbRead) return false;
	return g_wbRead(g_wbReadUser, p.c_str(), bytes);
}

}  // namespace lekmod_portable

namespace {

// ---- the file in memory as an FIFile (only Read / IsOpen / Close are used by the loaders) ---------------
class WbMemoryFile : public FIFile
{
public:
	explicit WbMemoryFile(const wchar_t* wszFilename) : m_pos(0), m_open(false)
	{
		m_open = lekmod_portable::ReadWbMapFile(wszFilename, m_data);
		if (!m_open) Log(LOG_WARN, "wbmap: cannot read '%s'", lekmod_portable::WbMapPath(wszFilename).c_str());
	}
	virtual ~WbMemoryFile() {}

	virtual FileErr Open(const char*, OpenMode, dword, uint) { return FILE_NOTFOUND; }
	virtual FileErr Open(const wchar*, OpenMode, dword, uint) { return FILE_NOTFOUND; }
	virtual FileErr Close(void) { m_open = false; return FILE_OK; }
	virtual const wchar_t* GetFileName() { return L""; }
	virtual byte* GetViewCaret(void) const { return NULL; }
	virtual byte* GetViewEnd(void) const { return NULL; }
	virtual byte* GetViewPtr(void) const { return NULL; }
	virtual uint GetLength(void) const { return (uint)m_data.size(); }
	virtual void SetLength(uint) {}
	virtual bool IsOpen(void) const { return m_open; }
	virtual bool IsMapped(void) const { return false; }
	virtual uint Seek(long lOffset, SeekMode eOrigin)
	{
		long base = eOrigin == begin ? 0 : eOrigin == current ? (long)m_pos : (long)m_data.size();
		long to = base + lOffset;
		m_pos = to < 0 ? 0 : (size_t)to > m_data.size() ? m_data.size() : (size_t)to;
		return (uint)m_pos;
	}
	virtual uint SeekToBegin() { m_pos = 0; return 0; }
	virtual uint SeekToEnd() { m_pos = m_data.size(); return (uint)m_pos; }
	virtual uint GetPosition() { return (uint)m_pos; }
	virtual void Rewind(void) { m_pos = 0; }
	virtual bool IsEOF(void) { return m_pos >= m_data.size(); }
	virtual uint Read(void* pData, uint size)
	{
		if (!m_open || !pData) return 0;
		const size_t n = m_pos + size > m_data.size() ? m_data.size() - m_pos : size;
		if (n) memcpy(pData, m_data.data() + m_pos, n);
		m_pos += n;
		return (uint)n;
	}
	virtual uint Write(const void*, uint) { return 0; }
	virtual uint Flush(void) { return 0; }

private:
	std::string m_data;
	size_t m_pos;
	bool m_open;
};

// ---- i386 file records -------------------------------------------------------------------------------
// Sizes of the records as the original writes them (sizeof on i386); the decoders below read the fields at these offsets.
const uint kMapHeaderSize = 9;              // byVersion, uiWidth, uiHeight (Load reads these; LoadN reads byPlayableCivCount)
const uint kPlotRecordSize = 8;             // PlotMapData, PlotScenarioData: bytes and words only, same on every target
const uint kTeamRecordSize = 0x40;
const uint kPlayerRecordSize = 0x1b4;
const uint kOldPlayerRecordSize = 0x130;
const uint kOldUnitRecordSize = 0xc;
const uint kUnitV2RecordSize = 0x30;
const uint kUnitRecordSize = 0x54;
const uint kUnitNameRecordSize = 0x40;
const uint kOldCityRecordSize = 0x64;
const uint kCityV2RecordSize = 0x68;
const uint kCityRecordSize = 0x88;
const uint kScenarioHeaderSize = 0x80;      // versions 8+: GameDescription (0x54, padded to 0x54) + 11 sizes
const uint kOldScenarioHeaderSize = 0x78;   // versions 6-7: GameDescription + 9 sizes (no promotions, no unit names)
const uint kMaxTypeTablesSize = 0x100000;   // Load11Plus: the type tables, mod data and strings of a map, at most 1 MB
const uint kMaxScenarioSize = 0x800000;     // Load11Plus: the scenario block, at most 8 MB

// Both targets are little-endian (x86_64, wasm32), as is the file.
inline uint U32(const byte* p) { uint v; memcpy(&v, p, 4); return v; }
inline int I32(const byte* p) { int v; memcpy(&v, p, 4); return v; }
inline word U16(const byte* p) { word v; memcpy(&v, p, 2); return v; }

// A bitset from its packed little-endian bytes (bit i = byte i / 8, bit i % 8): independent of the bitset word size.
template <size_t N>
void BitsFrom(std::bitset<N>& bits, const byte* p)
{
	bits.reset();
	for (size_t i = 0; i < N; ++i)
		if (p[i >> 3] & (1 << (i & 7))) bits.set(i);
}

// strcpy_s(dst, 64, src) of the record conversions; src is a fixed char array of srcLen bytes.
void CopyName(char* dst, size_t dstLen, const char* src, size_t srcLen)
{
	size_t n = 0;
	while (n < srcLen && src[n]) ++n;
	if (n >= dstLen) n = 0;  // strcpy_s fails and leaves an empty string
	memcpy(dst, src, n);
	dst[n] = '\0';
}

// (uint)(float): cvttss2si with the 2^31 adjustment, as the i386 code converts.
uint FloatToUint(float f)
{
	if (!(f >= 0.0f)) return (uint)(int)f;
	if (f >= 2147483648.0f) return (uint)(int)(f - 2147483648.0f) ^ 0x80000000u;
	return (uint)(int)f;
}

template <class H>
void SetHandle(H& h, const byte* p) { memcpy(&h, p, sizeof(word)); }

size_t BoundedLength(const byte* p, size_t n)
{
	size_t i = 0;
	while (i < n && p[i]) ++i;
	return i;
}

typedef CvWorldBuilderMap WBM;

void DecodeOldUnit(WBM::OldUnit& u, const byte* p)
{
	SetHandle(u.m_hStackedUnit, p + 0x0);
	u.m_wExperience = U16(p + 0x2);
	memcpy(&u.m_fHealth, p + 0x4, 4);
	u.m_byUnitType = p[0x8];
	u.m_byOwner = p[0x9];
	u.m_byDirection = p[0xa];
	u.m_bFortified = (p[0xb] & 1) != 0;
	u.m_bEmbarked = (p[0xb] & 2) != 0;
}

void DecodeUnitV2(WBM::UnitV2& u, const byte* p)
{
	SetHandle(u.m_hStackedUnit, p + 0x0);
	SetHandle(u.m_hCustomName, p + 0x2);
	u.m_uiExperience = U32(p + 0x4);
	u.m_uiHealth = U32(p + 0x8);
	u.m_byUnitType = p[0xc];
	u.m_byOwner = p[0xd];
	u.m_byDirection = p[0xe];
	u.m_byFlags = p[0xf];
	BitsFrom(u.m_kPromotions, p + 0x10);
}

void DecodeUnit(WBM::Unit& u, const byte* p)
{
	SetHandle(u.m_hStackedUnit, p + 0x0);
	SetHandle(u.m_hCustomName, p + 0x2);
	u.m_uiExperience = U32(p + 0x4);
	u.m_uiHealth = U32(p + 0x8);
	u.m_byUnitType = U32(p + 0xc);
	u.m_byOwner = p[0x10];
	u.m_byDirection = p[0x11];
	u.m_byFlags = p[0x12];
	BitsFrom(u.m_kPromotions, p + 0x14);
}

void DecodeOldCity(WBM::OldCity& c, const byte* p)
{
	memcpy(c.m_szName, p + 0x0, sizeof(c.m_szName));
	c.m_uiPopulation = U32(p + 0x38);
	c.m_uiCulture = U32(p + 0x3c);
	c.m_byOwner = p[0x40];
	c.m_bLocalizedName = p[0x41] != 0;
	BitsFrom(c.m_kBuildings, p + 0x44);
}

void DecodeCityV2(WBM::CityV2& c, const byte* p)
{
	memcpy(c.m_szName, p + 0x0, sizeof(c.m_szName));
	c.m_byOwner = p[0x40];
	c.m_byFlags = p[0x41];
	c.m_wPopulation = U16(p + 0x42);
	c.m_uiHealth = U32(p + 0x44);
	BitsFrom(c.m_kBuildings, p + 0x48);
}

void DecodeCity(WBM::City& c, const byte* p)
{
	memcpy(c.m_szName, p + 0x0, sizeof(c.m_szName));
	c.m_byOwner = p[0x40];
	c.m_byFlags = p[0x41];
	c.m_wPopulation = U16(p + 0x42);
	c.m_uiHealth = U32(p + 0x44);
	BitsFrom(c.m_kBuildings, p + 0x48);
}

void DecodeTeam(WBM::Team& t, const byte* p)
{
	memcpy(t.m_szName, p + 0x0, sizeof(t.m_szName));
	BitsFrom(t.m_kTechs, p + 0x20);
}

void DecodePlayer(WBM::Player& pl, const byte* p)
{
	BitsFrom(pl.m_kPolicies, p + 0x0);
	memcpy(pl.m_szLeaderName, p + 0x20, 64);
	memcpy(pl.m_szCivName, p + 0x60, 64);
	memcpy(pl.m_szCivType, p + 0xa0, 64);
	memcpy(pl.m_szTeamColor, p + 0xe0, 64);
	memcpy(pl.m_szEra, p + 0x120, 64);
	memcpy(pl.m_szHandicap, p + 0x160, 64);
	pl.m_uiCulture = U32(p + 0x1a0);
	pl.m_uiGold = U32(p + 0x1a4);
	pl.m_uiStartX = U32(p + 0x1a8);
	pl.m_uiStartY = U32(p + 0x1ac);
	pl.m_byTeam = p[0x1b0];
	pl.m_bPlayable = p[0x1b1] != 0;
}

void DecodeOldPlayer(WBM::OldPlayer& pl, const byte* p)
{
	BitsFrom(pl.m_kPolicies, p + 0x0);
	memcpy(pl.m_szCivType, p + 0x20, 64);
	memcpy(pl.m_szTeamColor, p + 0x60, 64);
	memcpy(pl.m_szEra, p + 0xa0, 64);
	memcpy(pl.m_szHandicap, p + 0xe0, 60);
	pl.m_uiCulture = U32(p + 0x11c);
	pl.m_uiGold = U32(p + 0x120);
	pl.m_uiStartX = U32(p + 0x124);
	pl.m_uiStartY = U32(p + 0x128);
	pl.m_byTeam = p[0x12c];
	pl.m_bPlayable = p[0x12d] != 0;
}

// An element allocator block of the file (int free-list head, then fixed-size entries) -> the host layout of Map<T>, ready for
// Map::Deserialize. Raw: every entry keeps its first bytes (the free-list link of a free entry) and is decoded as T (Deserialize
// in the original). Converted: the free entries are found by the walk of CvWorldBuilderMapElementAllocator::ConvertTo
// (index 0 is never marked free there) and keep only the link; the others are decoded as the old record and converted to T.
template <class T>
class AllocatorImage
{
public:
	std::vector<byte> bytes;
	uint count;

	AllocatorImage(const byte* p, uint size, uint recordSize) : count(0), m_p(p), m_size(size), m_record(recordSize)
	{
		if (size < sizeof(int) || recordSize == 0) return;
		count = (size - sizeof(int)) / recordSize;
		bytes.assign(sizeof(int) + (size_t)count * sizeof(T), 0);
		memcpy(&bytes[0], p, sizeof(int));
	}
	const byte* Record(uint i) const { return m_p + sizeof(int) + (size_t)i * m_record; }
	void Put(uint i, const T& value) { memcpy(&bytes[sizeof(int) + (size_t)i * sizeof(T)], &value, sizeof(T)); }
	void PutLink(uint i) { memcpy(&bytes[sizeof(int) + (size_t)i * sizeof(T)], Record(i), sizeof(int)); }
	std::vector<bool> FreeEntries() const
	{
		std::vector<bool> free(count, false);
		int iFree = I32(m_p);
		for (uint steps = 0; iFree > 0 && iFree < (int)count && steps < count; ++steps)
		{
			free[(size_t)iFree] = true;
			iFree = I32(Record((uint)iFree));
		}
		return free;
	}
	template <class M>
	void Into(M& map) { if (count == 0) map.Release(); else map.Deserialize(&bytes[0], (uint)bytes.size()); }

private:
	const byte* m_p;
	uint m_size;
	uint m_record;
};

void LoadUnits(WBM::Unit::Map& units, const byte* p, uint size, byte byVersion)
{
	if (byVersion >= 12)
	{
		AllocatorImage<WBM::Unit> img(p, size, kUnitRecordSize);
		for (uint i = 0; i < img.count; ++i)
		{
			WBM::Unit u;
			DecodeUnit(u, img.Record(i));
			img.Put(i, u);
		}
		img.Into(units);
		return;
	}
	const uint record = byVersion >= 8 ? kUnitV2RecordSize : kOldUnitRecordSize;
	AllocatorImage<WBM::Unit> img(p, size, record);
	const std::vector<bool> free = img.FreeEntries();
	for (uint i = 0; i < img.count; ++i)
	{
		if (free[i]) { img.PutLink(i); continue; }
		if (byVersion >= 8)
		{
			WBM::UnitV2 old;
			DecodeUnitV2(old, img.Record(i));
			img.Put(i, WBM::Unit(old));
		}
		else
		{
			WBM::OldUnit old;
			DecodeOldUnit(old, img.Record(i));
			img.Put(i, WBM::Unit(old));
		}
	}
	img.Into(units);
}

void LoadCities(WBM::City::Map& cities, const byte* p, uint size, byte byVersion)
{
	if (byVersion >= 12)
	{
		AllocatorImage<WBM::City> img(p, size, kCityRecordSize);
		for (uint i = 0; i < img.count; ++i)
		{
			WBM::City c;
			DecodeCity(c, img.Record(i));  // a free entry's link is in the name's first bytes
			img.Put(i, c);
		}
		img.Into(cities);
		return;
	}
	const uint record = byVersion >= 8 ? kCityV2RecordSize : kOldCityRecordSize;
	AllocatorImage<WBM::City> img(p, size, record);
	const std::vector<bool> free = img.FreeEntries();
	for (uint i = 0; i < img.count; ++i)
	{
		if (free[i]) { img.PutLink(i); continue; }
		if (byVersion >= 8)
		{
			WBM::CityV2 old;
			DecodeCityV2(old, img.Record(i));
			img.Put(i, WBM::City(old));
		}
		else
		{
			WBM::OldCity old;
			DecodeOldCity(old, img.Record(i));
			img.Put(i, WBM::City(old));
		}
	}
	img.Into(cities);
}

void LoadUnitNames(WBM::UnitName::Map& names, const byte* p, uint size)
{
	AllocatorImage<WBM::UnitName> img(p, size, kUnitNameRecordSize);
	for (uint i = 0; i < img.count; ++i)
	{
		WBM::UnitName n;
		memcpy(n.m_szText, img.Record(i), sizeof(n.m_szText));
		img.Put(i, n);
	}
	img.Into(names);
}

typedef CvWorldBuilderMapTypeDesc::LookupTable Lookup;

// Index through a lookup table; out of the table -> fallback.
inline uint Remap(const Lookup& table, uint index, uint fallback) { return index < table.size() ? table[index] : fallback; }

// A type bitset of the file's numbering -> the game's (bits beyond the table or mapped beyond the bitset are dropped).
template <size_t N>
void RemapBits(std::bitset<N>& bits, const Lookup& table)
{
	std::bitset<N> out;
	const size_t n = table.size() < N ? table.size() : N;
	for (size_t i = 0; i < n; ++i)
	{
		const uint to = table[i];
		if (to < N) out.set(to, bits[i]);
	}
	bits = out;
}

void DeserializeTypes(CvWorldBuilderMapTypeDesc::TypeMap& types, byte* p, uint size)
{
	if (size) types.Deserialize(p, size);
}

}  // namespace

// ---- record conversions -----------------------------------------------
CvWorldBuilderMap::Unit::Unit(const OldUnit& kOldUnit)
{
	new(this) Unit();
	memcpy(&m_hStackedUnit, &kOldUnit.m_hStackedUnit, sizeof(word));  // the handle types of the two allocators differ
	m_uiExperience = kOldUnit.m_wExperience;
	m_uiHealth = FloatToUint(kOldUnit.m_fHealth * (float)MaxHealth);
	m_byUnitType = kOldUnit.m_byUnitType;
	m_byOwner = kOldUnit.m_byOwner;
	m_byDirection = kOldUnit.m_byDirection;
	m_byFlags = (kOldUnit.m_bFortified ? UNIT_FORTIFIED : 0) | (kOldUnit.m_bEmbarked ? UNIT_EMBARKED : 0);
}

// The custom name handle is not carried over (as in the original).
CvWorldBuilderMap::Unit::Unit(const UnitV2& kOldUnit)
{
	new(this) Unit();
	memcpy(&m_hStackedUnit, &kOldUnit.m_hStackedUnit, sizeof(word));  // the handle types of the two allocators differ
	m_uiExperience = kOldUnit.m_uiExperience;
	m_uiHealth = kOldUnit.m_uiHealth;
	m_byUnitType = kOldUnit.m_byUnitType;
	m_byOwner = kOldUnit.m_byOwner;
	m_byDirection = kOldUnit.m_byDirection;
	m_byFlags = kOldUnit.m_byFlags;
	for (uint i = 0; i < UnitV2::MaxPromotionTypes; ++i) m_kPromotions.set(i, kOldUnit.m_kPromotions[i]);
}

// The buildings are not carried over (as in the original); the localized flag is the old bool byte.
CvWorldBuilderMap::City::City(const OldCity& kOldCity)
{
	new(this) City();
	m_byOwner = kOldCity.m_byOwner;
	m_byFlags = kOldCity.m_bLocalizedName ? 1 : 0;
	m_wPopulation = (word)kOldCity.m_uiPopulation;
	m_uiHealth = MaxHealth;
	CopyName(m_szName, sizeof(m_szName), kOldCity.m_szName, sizeof(kOldCity.m_szName));
}

CvWorldBuilderMap::City::City(const CityV2& kOldCity)
{
	new(this) City();
	m_byOwner = kOldCity.m_byOwner;
	m_byFlags = kOldCity.m_byFlags;
	m_wPopulation = kOldCity.m_wPopulation;
	m_uiHealth = kOldCity.m_uiHealth;
	CopyName(m_szName, sizeof(m_szName), kOldCity.m_szName, sizeof(kOldCity.m_szName));
	for (uint i = 0; i < CityV2::MaxBuildingTypes; ++i) m_kBuildings.set(i, kOldCity.m_kBuildings[i]);
}

// The policies are not carried over (as in the original).
CvWorldBuilderMap::Player::Player(const OldPlayer& kOldPlayer)
{
	new(this) Player();
	m_uiCulture = kOldPlayer.m_uiCulture;
	m_uiGold = kOldPlayer.m_uiGold;
	m_uiStartX = kOldPlayer.m_uiStartX;
	m_uiStartY = kOldPlayer.m_uiStartY;
	m_byTeam = kOldPlayer.m_byTeam;
	m_bPlayable = kOldPlayer.m_bPlayable;
	CopyName(m_szCivType, sizeof(m_szCivType), kOldPlayer.m_szCivType, sizeof(kOldPlayer.m_szCivType));
	CopyName(m_szTeamColor, sizeof(m_szTeamColor), kOldPlayer.m_szTeamColor, sizeof(kOldPlayer.m_szTeamColor));
	CopyName(m_szEra, sizeof(m_szEra), kOldPlayer.m_szEra, sizeof(kOldPlayer.m_szEra));
	CopyName(m_szHandicap, sizeof(m_szHandicap), kOldPlayer.m_szHandicap, sizeof(kOldPlayer.m_szHandicap));
}

// ---- Load ------------------------------------------------------------------------------------------------
bool CvWorldBuilderMap::Load(const wchar_t* wszFilename, const CvWorldBuilderMapTypeDesc& kTypeDesc, bool bScenario)
{
	WbMemoryFile kFile(wszFilename);
	if (!kFile.IsOpen()) return false;

	byte abyHeader[kMapHeaderSize];
	bool bLoaded = false;
	if (kFile.Read(abyHeader, kMapHeaderSize) >= kMapHeaderSize)
	{
		const byte byVersion = abyHeader[0];
		const uint uiWidth = U32(abyHeader + 1), uiHeight = U32(abyHeader + 5);
		if (uiHeight * uiWidth <= MaxMapSize)
		{
			m_kMapDesc.uiWidth = uiWidth;
			m_kMapDesc.uiHeight = uiHeight;
			m_sMapName.Empty();
			m_sMapDescription.Empty();

			const byte byFileVersion = byVersion & 0x7f;
			const bool bFileHasScenario = (byVersion & 0x80) != 0;
			ZeroMemory(m_szWorldType, sizeof(m_szWorldType));
			if (byFileVersion <= FileVersion)
			{
				const bool bLoadScenario = bFileHasScenario && bScenario;
				switch (byFileVersion)
				{
				case 0: bLoaded = Load0(kFile, kTypeDesc, bLoadScenario); break;
				case 1: bLoaded = Load1(kFile, kTypeDesc, bLoadScenario); break;
				case 2: bLoaded = Load2(kFile, kTypeDesc, bLoadScenario); break;
				case 3: bLoaded = Load3(kFile, kTypeDesc, bLoadScenario); break;
				case 4: bLoaded = Load4(kFile, kTypeDesc, bLoadScenario); break;
				case 5: bLoaded = Load5(kFile, kTypeDesc, bLoadScenario); break;
				case 6: bLoaded = Load6(kFile, kTypeDesc, bLoadScenario); break;
				case 7: bLoaded = Load7(kFile, kTypeDesc, bLoadScenario); break;
				case 8: bLoaded = Load8(kFile, kTypeDesc, bLoadScenario); break;
				case 9: bLoaded = Load9(kFile, kTypeDesc, bLoadScenario); break;
				case 10: bLoaded = Load10(kFile, kTypeDesc, bLoadScenario); break;
				default: bLoaded = Load11Plus(kFile, kTypeDesc, bLoadScenario, byFileVersion); break;
				}

				if (bLoaded && !bFileHasScenario && bScenario)
					ClearScenarioData(false);
			}
			else
				Log(LOG_WARN, "wbmap: '%s' has file version %u, newer than %u", lekmod_portable::WbMapPath(wszFilename).c_str(), byFileVersion, FileVersion);
			if (!bLoaded) Log(LOG_WARN, "wbmap: '%s' (version %u) did not load", lekmod_portable::WbMapPath(wszFilename).c_str(), byFileVersion);
		}
	}

	kFile.Close();
	return bLoaded;
}

// Versions 0-5 predate the shipped maps (the oldest in the game is version 6, Tutorials/Maps/UnitBenchmark.Civ5Map).
#define LEKMOD_WB_UNSUPPORTED(N) \
	bool CvWorldBuilderMap::Load##N(FIFile&, const CvWorldBuilderMapTypeDesc&, bool) \
	{ \
		Log(LOG_WARN, "wbmap: file version " #N " is not supported (6-12 are)"); \
		return false; \
	}
LEKMOD_WB_UNSUPPORTED(0)
LEKMOD_WB_UNSUPPORTED(1)
LEKMOD_WB_UNSUPPORTED(2)
LEKMOD_WB_UNSUPPORTED(3)
LEKMOD_WB_UNSUPPORTED(4)
LEKMOD_WB_UNSUPPORTED(5)
#undef LEKMOD_WB_UNSUPPORTED

bool CvWorldBuilderMap::Load6(FIFile& kFile, const CvWorldBuilderMapTypeDesc& kTypeDesc, bool bScenario) { return Load11Plus(kFile, kTypeDesc, bScenario, 6); }
bool CvWorldBuilderMap::Load7(FIFile& kFile, const CvWorldBuilderMapTypeDesc& kTypeDesc, bool bScenario) { return Load11Plus(kFile, kTypeDesc, bScenario, 7); }
bool CvWorldBuilderMap::Load8(FIFile& kFile, const CvWorldBuilderMapTypeDesc& kTypeDesc, bool bScenario) { return Load11Plus(kFile, kTypeDesc, bScenario, 8); }
bool CvWorldBuilderMap::Load9(FIFile& kFile, const CvWorldBuilderMapTypeDesc& kTypeDesc, bool bScenario) { return Load11Plus(kFile, kTypeDesc, bScenario, 9); }
bool CvWorldBuilderMap::Load10(FIFile& kFile, const CvWorldBuilderMapTypeDesc& kTypeDesc, bool bScenario) { return Load11Plus(kFile, kTypeDesc, bScenario, 10); }

// File layout after the 9 header bytes (all integers little-endian u32 unless noted; "v" = file version):
//   byPlayableCivCount (u8)
//   uiFlags, then the sizes of: terrain, feature, natural wonder, resource type tables, mod data, map name, map description (v6: no
//   description) -- the tables are NUL-separated type names, the strings NUL-terminated
//   v11+: world type size (<= 64) + the world size type name
//   width * height PlotMapData (8 bytes)
//   scenario files only: GameDescription (speed[64], starting turn, max turns, target score, start year, player / city-state / team
//   counts) and the block sizes (0x80 bytes; v6-7: 0x78, no promotion and unit name tables), then improvement, unit, tech, policy,
//   building, promotion type tables, units, unit names, cities (element allocators), victory types and game options (length-prefixed
//   names), 5 team relationship bitsets, city-state influence (v10+: player count * 64 ints), team visibility (teams * plots bits),
//   then Team records, Player records (v6-8: OldPlayer), city-state Player records and width * height PlotScenarioData.
bool CvWorldBuilderMap::Load11Plus(FIFile& kFile, const CvWorldBuilderMapTypeDesc& kTypeDesc, bool bScenario, byte byFileVersion)
{
	const byte v = byFileVersion;
	m_uiFlags = FLAG_WORLD_WRAP | FLAG_TRANSFERABLE;

	byte byPlayableCivCount = 0;
	if (kFile.Read(&byPlayableCivCount, 1) != 1) return false;

	// flags, terrain, feature, wonder, resource, mod data, name, description
	uint auiSizes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	const uint uiSizeCount = v >= 7 ? 8 : 7;
	byte abySizes[8 * 4];
	if (kFile.Read(abySizes, uiSizeCount * 4) < uiSizeCount * 4) return false;
	for (uint i = 0; i < uiSizeCount; ++i) auiSizes[i] = U32(abySizes + 4 * i);
	uint uiTypesSize = 0;
	for (uint i = 1; i < 8; ++i) uiTypesSize += auiSizes[i];
	if (uiTypesSize == 0 || uiTypesSize > kMaxTypeTablesSize) return false;
	m_uiFlags = auiSizes[0];

	std::vector<byte> kTypes(uiTypesSize + 1, 0);
	if (kFile.Read(&kTypes[0], uiTypesSize) < uiTypesSize) return false;

	CvWorldBuilderMapTypeDesc kFileTypes;
	byte* pbyCursor = &kTypes[0];
	DeserializeTypes(kFileTypes.m_kTerrainTypes, pbyCursor, auiSizes[1]);
	pbyCursor += auiSizes[1];
	DeserializeTypes(kFileTypes.m_kFeatures, pbyCursor, auiSizes[2]);
	pbyCursor += auiSizes[2];
	DeserializeTypes(kFileTypes.m_kNaturalWonders, pbyCursor, auiSizes[3]);
	pbyCursor += auiSizes[3];
	DeserializeTypes(kFileTypes.m_kResources, pbyCursor, auiSizes[4]);
	pbyCursor += auiSizes[4];
	if (auiSizes[5] != 0)
		m_kModData.Deserialize(pbyCursor);
	else
		m_kModData.ResetTypes();
	pbyCursor += auiSizes[5];
	// The original copies strlen() bytes from each string's start; a string without a terminator stops at the block's end here.
	m_sMapName.Copy((int)BoundedLength(pbyCursor, auiSizes[6]), (const char*)pbyCursor);
	pbyCursor += auiSizes[6];
	m_sMapDescription.Copy((int)BoundedLength(pbyCursor, auiSizes[7]), (const char*)pbyCursor);

	if (v >= 11)
	{
		byte abyWorldTypeSize[4];
		if (kFile.Read(abyWorldTypeSize, 4) < 4) return false;
		const uint uiWorldTypeSize = U32(abyWorldTypeSize);
		if (uiWorldTypeSize > sizeof(m_szWorldType)) return false;
		kFile.Read(m_szWorldType, uiWorldTypeSize);
		m_szWorldType[sizeof(m_szWorldType) - 1] = '\0';
	}

	const uint uiPlotCount = m_kMapDesc.uiWidth * m_kMapDesc.uiHeight;
	const uint uiPlotDataSize = uiPlotCount * kPlotRecordSize;
	if (kFile.Read(m_akMapData, uiPlotDataSize) < uiPlotDataSize) return false;

	if (bScenario)
	{
		// v6-7 start from cleared scenario data; later versions overwrite every part they use.
		if (v <= 7) ClearScenarioData(false);

		const uint uiHeaderSize = v >= 8 ? kScenarioHeaderSize : kOldScenarioHeaderSize;
		byte abyHeader[kScenarioHeaderSize];
		memset(abyHeader, 0, sizeof(abyHeader));
		const uint uiHeaderRead = kFile.Read(abyHeader, uiHeaderSize);
		memcpy(m_kGameDesc.szDefaultSpeed, abyHeader, sizeof(m_kGameDesc.szDefaultSpeed));
		m_kGameDesc.uiStartingTurn = U32(abyHeader + 0x40);
		m_kGameDesc.uiMaxTurns = U32(abyHeader + 0x44);
		m_kGameDesc.uiTargetScore = U32(abyHeader + 0x48);
		m_kGameDesc.iStartYear = I32(abyHeader + 0x4c);
		m_kGameDesc.byPlayerCount = abyHeader[0x50];
		m_kGameDesc.byCityStateCount = abyHeader[0x51];
		m_kGameDesc.byTeamCount = abyHeader[0x52];
		if (uiHeaderRead < uiHeaderSize) return false;

		// improvements, unit types, techs, policies, buildings, promotions, units, unit names, cities, victory types, game options
		enum { IMPR, UNIT_TYPES, TECHS, POLICIES, BUILDINGS, PROMOTIONS, UNITS, UNIT_NAMES, CITIES, VICTORIES, OPTIONS, BLOCKS };
		uint auiBlock[BLOCKS];
		memset(auiBlock, 0, sizeof(auiBlock));
		const byte* pbySizes = abyHeader + 0x54;
		if (v >= 8)
			for (uint i = 0; i < BLOCKS; ++i) auiBlock[i] = U32(pbySizes + 4 * i);
		else
		{
			const uint aiOld[] = {IMPR, UNIT_TYPES, TECHS, POLICIES, BUILDINGS, UNITS, CITIES, VICTORIES, OPTIONS};
			for (uint i = 0; i < sizeof(aiOld) / sizeof(aiOld[0]); ++i) auiBlock[aiOld[i]] = U32(pbySizes + 4 * i);
		}

		const uint uiTeamCount = m_kGameDesc.byTeamCount;
		const uint uiPlayerCount = m_kGameDesc.byPlayerCount;
		const uint uiCityStateCount = m_kGameDesc.byCityStateCount;
		const uint uiRelationshipSize = FRelationshipBitset<MaxTeams, true>::GetSerializedSize(uiTeamCount);
		// City-state influence is in the file from v10 on. Load8 / Load9 copy player count * 256 bytes from where it would be and the
		// visibility from past the end of their buffer (heap memory); here the influence of v8-9 stays zero and the visibility is the file's.
		const uint uiInfluenceSize = v >= 10 ? uiPlayerCount * sizeof(m_aiCityStateInfluence[0]) : 0;
		const uint uiVisibilityBitCount = uiTeamCount * uiPlotCount;
		const uint uiVisibilitySize = SerializableBitset<MaxMapSize * MaxTeams>::GetSerializedSize(uiVisibilityBitCount);
		uint uiScenarioSize = uiInfluenceSize + uiVisibilitySize + 5 * uiRelationshipSize;
		for (uint i = 0; i < BLOCKS; ++i) uiScenarioSize += auiBlock[i];
		if (uiScenarioSize > kMaxScenarioSize) return false;

		std::vector<byte> kScenario(uiScenarioSize + 1, 0);
		if (kFile.Read(&kScenario[0], uiScenarioSize) < uiScenarioSize) return false;
		byte* p = &kScenario[0];
		DeserializeTypes(kFileTypes.m_kImprovements, p, auiBlock[IMPR]);
		p += auiBlock[IMPR];
		DeserializeTypes(kFileTypes.m_kUnits, p, auiBlock[UNIT_TYPES]);
		p += auiBlock[UNIT_TYPES];
		DeserializeTypes(kFileTypes.m_kTechs, p, auiBlock[TECHS]);
		p += auiBlock[TECHS];
		DeserializeTypes(kFileTypes.m_kPolicies, p, auiBlock[POLICIES]);
		p += auiBlock[POLICIES];
		DeserializeTypes(kFileTypes.m_kBuildingTypes, p, auiBlock[BUILDINGS]);
		p += auiBlock[BUILDINGS];
		DeserializeTypes(kFileTypes.m_kUnitPromotions, p, auiBlock[PROMOTIONS]);
		p += auiBlock[PROMOTIONS];
		LoadUnits(m_kUnits, p, auiBlock[UNITS], v);
		p += auiBlock[UNITS];
		if (v >= 8) LoadUnitNames(m_kUnitNames, p, auiBlock[UNIT_NAMES]);
		p += auiBlock[UNIT_NAMES];
		LoadCities(m_kCities, p, auiBlock[CITIES], v);
		p += auiBlock[CITIES];
		m_kVictoryTypes.Deserialize(p, auiBlock[VICTORIES]);
		p += auiBlock[VICTORIES];
		m_kGameOptions.Deserialize(p, auiBlock[OPTIONS]);
		p += auiBlock[OPTIONS];
		m_kTeamsInContact.Deserialize(p, uiTeamCount);
		p += uiRelationshipSize;
		m_kTeamsAtWar.Deserialize(p, uiTeamCount);
		p += uiRelationshipSize;
		m_kTeamsAtPermanentWarOrPeace.Deserialize(p, uiTeamCount);
		p += uiRelationshipSize;
		m_kTeamsSharingOpenBorders.Deserialize(p, uiTeamCount);
		p += uiRelationshipSize;
		m_kTeamsSharingDefensivePacts.Deserialize(p, uiTeamCount);
		p += uiRelationshipSize;
		if (v >= 10)
			memcpy(m_aiCityStateInfluence, p, uiInfluenceSize);
		else
			ZeroMemory(m_aiCityStateInfluence, sizeof(m_aiCityStateInfluence));
		p += uiInfluenceSize;
		m_kVisibility.Deserialize(p, uiVisibilityBitCount);

		std::vector<byte> kRecords(uiTeamCount * kTeamRecordSize + 1);
		if (kFile.Read(&kRecords[0], uiTeamCount * kTeamRecordSize) < uiTeamCount * kTeamRecordSize) return false;
		for (uint i = 0; i < uiTeamCount && i < MaxTeams; ++i) DecodeTeam(m_akTeams[i], &kRecords[i * kTeamRecordSize]);

		const uint uiPlayerRecord = v >= 9 ? kPlayerRecordSize : kOldPlayerRecordSize;
		for (uint uiList = 0; uiList < 2; ++uiList)
		{
			const uint uiCount = uiList == 0 ? uiPlayerCount : uiCityStateCount;
			Player* akPlayers = uiList == 0 ? m_akPlayers : m_akCityStates;
			const uint uiMax = uiList == 0 ? MaxPlayers : MaxCityStates;
			kRecords.assign(uiCount * uiPlayerRecord + 1, 0);
			if (kFile.Read(&kRecords[0], uiCount * uiPlayerRecord) < uiCount * uiPlayerRecord) return false;
			for (uint i = 0; i < uiCount && i < uiMax; ++i)
			{
				if (v >= 9)
					DecodePlayer(akPlayers[i], &kRecords[i * uiPlayerRecord]);
				else
				{
					OldPlayer kOld;
					DecodeOldPlayer(kOld, &kRecords[i * uiPlayerRecord]);
					akPlayers[i] = Player(kOld);
				}
			}
		}

		if (kFile.Read(m_akScenarioData, uiPlotDataSize) < uiPlotDataSize) return false;
	}

	// The file's type numbering -> the game's (CvWorldBuilderMapTypeDesc::BuildLookupTable: by type name; a name the game lacks -> -1).
	Lookup kTerrain, kFeatures, kWonders, kResources;
	const bool bTerrain = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kTerrainTypes, kTypeDesc.m_kTerrainTypes, kTerrain);
	const bool bFeatures = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kFeatures, kTypeDesc.m_kFeatures, kFeatures);
	const bool bWonders = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kNaturalWonders, kTypeDesc.m_kNaturalWonders, kWonders);
	const bool bResources = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kResources, kTypeDesc.m_kResources, kResources);
	if (uiPlotCount != 0 && (bTerrain || bFeatures || bWonders || bResources))
	{
		for (uint i = 0; i < uiPlotCount; ++i)
		{
			PlotMapData& kPlot = m_akMapData[i];
			kPlot.SetTerrainType((byte)Remap(kTerrain, kPlot.GetTerrainType(), 0));
			kPlot.SetFeatureType((byte)Remap(kFeatures, kPlot.GetFeatureType(), PlotMapData::InvalidFeature));
			kPlot.SetNaturalWonderType((byte)Remap(kWonders, kPlot.GetNaturalWonderType(), PlotMapData::InvalidNaturalWonder));
			kPlot.SetResourceType((byte)Remap(kResources, kPlot.GetResourceType(), PlotMapData::InvalidResource));
		}
	}

	if (bScenario)
	{
		Lookup kImprovements, kBuildings, kTechs, kPolicies, kUnitTypes, kPromotions;
		const bool bImprovements = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kImprovements, kTypeDesc.m_kImprovements, kImprovements);
		const bool bBuildings = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kBuildingTypes, kTypeDesc.m_kBuildingTypes, kBuildings);
		if (uiPlotCount != 0 && (bImprovements || bBuildings))
		{
			for (uint i = 0; i < uiPlotCount; ++i)
			{
				PlotScenarioData& kPlot = m_akScenarioData[i];
				kPlot.m_byImprovement = (byte)Remap(kImprovements, kPlot.m_byImprovement, PlotScenarioData::InvalidImprovement);
				City* pkCity = bBuildings ? m_kCities[kPlot.m_hCity] : NULL;
				if (pkCity) RemapBits(pkCity->m_kBuildings, kBuildings);
			}
		}

		const bool bTechs = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kTechs, kTypeDesc.m_kTechs, kTechs);
		if (bTechs)
			for (uint i = 0; i < m_kGameDesc.byTeamCount && i < MaxTeams; ++i) RemapBits(m_akTeams[i].m_kTechs, kTechs);

		// Only the major civilizations' policies (as in the original).
		const bool bPolicies = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kPolicies, kTypeDesc.m_kPolicies, kPolicies);
		if (bPolicies)
			for (uint i = 0; i < m_kGameDesc.byPlayerCount && i < MaxPlayers; ++i) RemapBits(m_akPlayers[i].m_kPolicies, kPolicies);

		// Units are reached through the plots' unit stacks.
		const bool bUnitTypes = CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kUnits, kTypeDesc.m_kUnits, kUnitTypes);
		const bool bPromotions = v >= 8 && CvWorldBuilderMapTypeDesc::BuildLookupTable(kFileTypes.m_kUnitPromotions, kTypeDesc.m_kUnitPromotions, kPromotions);
		if (bUnitTypes || bPromotions)
		{
			for (uint i = 0; i < uiPlotCount; ++i)
			{
				uint uiSteps = 0;
				for (Unit* pkUnit = m_kUnits[m_akScenarioData[i].m_hUnitStack]; pkUnit != NULL && uiSteps <= 0xffff; pkUnit = m_kUnits[pkUnit->m_hStackedUnit], ++uiSteps)
				{
					pkUnit->m_byUnitType = Remap(kUnitTypes, pkUnit->m_byUnitType, Unit::InvalidUnitType);
					if (bPromotions) RemapBits(pkUnit->m_kPromotions, kPromotions);
				}
			}
		}
	}
	return true;
}

uint CvWorldBuilderMap::PreviewPlayableCivCount(const wchar_t* wszFilename)
{
	WbMemoryFile kFile(wszFilename);
	uint uiPlayableCount = 0;
	if (kFile.IsOpen())
	{
		byte abyHeader[kMapHeaderSize + 1];
		const uint uiRead = kFile.Read(abyHeader, sizeof(abyHeader));
		kFile.Close();

		const byte byFileVersion = abyHeader[0] & 0x7f;
		// Only scenario files from version 3 on carry a playable civ count
		if (uiRead == sizeof(abyHeader) && (abyHeader[0] & 0x80) != 0 && byFileVersion >= 3)
		{
			if (byFileVersion <= 4)
			{
				// Versions 3 and 4 do not store the count in the header, so the whole map is loaded
				CvWorldBuilderMap* pkMap = new CvWorldBuilderMap();
				CvWorldBuilderMapTypeDesc kTypeDesc;
				pkMap->Load(wszFilename, kTypeDesc, true);
				for (byte byPlayer = 0; byPlayer < pkMap->GetPlayerCount(); ++byPlayer)
					uiPlayableCount += pkMap->GetPlayer(byPlayer).m_bPlayable;
				delete pkMap;
			}
			else
				uiPlayableCount = abyHeader[kMapHeaderSize];
		}
	}
	return uiPlayableCount;
}
