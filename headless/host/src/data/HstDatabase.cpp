//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDatabase.cpp
//!  \brief     B1-B4: gameplay DB in memory, localization attach + init.
//!
//!		Mirrors exe LoadCoreDatabase (RVA 0x3664f0: BinaryIO(...).Load(App+0x944), "ATTACH ? AS Localization")
//!		and LocalizationInit (RVA 0x152d20: Localization::Initialize(icudt44l.dat, db, "Languages", lang, ...)).
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "data/HstDatabase.h"
#include "platform/HstPlatform.h"

HstDatabase::HstDatabase()
	: m_pkDb(NULL)
	, m_bLocalization(false)
{
}

HstDatabase::~HstDatabase()
{
	// The DLL may still reference the connection at process exit; it is intentionally leaked.
}

bool HstDatabase::OpenGameplay(const std::string& strUtf8GameplayDb)
{
	m_pkDb = new Database::Connection();
	// B1. GUESS (":memory:"): the exe's app connection is opened before the binary load.
	if(!m_pkDb->Open(":memory:", Database::OPEN_READWRITE | Database::OPEN_CREATE))
	{
		HstLog("db: Open(:memory:) failed: %s", m_pkDb->ErrorMessage());
		return false;
	}
	// B2. BinaryIO keeps the filename pointer; the string outlives the call.
	Database::BinaryIO kIO(strUtf8GameplayDb.c_str());
	if(!kIO.Load(*m_pkDb))
	{
		HstLog("db: BinaryIO::Load(%s) failed: %s", strUtf8GameplayDb.c_str(), m_pkDb->ErrorMessage());
		return false;
	}
	// Keep SQLite temp tables/sorts in memory so nothing is written to %TEMP% (file sandbox).
	m_pkDb->Execute("PRAGMA temp_store = MEMORY");
	return true;
}

bool HstDatabase::AttachLocalization(const std::string& strUtf8LocalizationDb)
{
	Database::Results kAttach;
	if(!m_pkDb->Execute(kAttach, "ATTACH ? AS Localization"))
	{
		HstLog("db: prepare ATTACH failed: %s", m_pkDb->ErrorMessage());
		return false;
	}
	kAttach.Bind(1, strUtf8LocalizationDb.c_str());
	if(!kAttach.Execute())
	{
		HstLog("db: ATTACH %s failed: %s", strUtf8LocalizationDb.c_str(), m_pkDb->ErrorMessage());
		return false;
	}
	kAttach.Release();
	return true;
}

bool HstDatabase::InitLocalization(const std::wstring& wstrIcuDat, const char* szLanguage)
{
	m_bLocalization = Localization::Initialize(wstrIcuDat.c_str(), *m_pkDb, "Languages", szLanguage, false);
	if(!m_bLocalization)
		HstLog("db: Localization::Initialize failed");
	return m_bLocalization;
}

int HstDatabase::Count(const char* szTable)
{
	return m_pkDb ? m_pkDb->Count(szTable, false) : -1;
}

bool HstDatabase::QueryText(const char* szSql, std::string& strOut)
{
	Database::Results kResults;
	if(!m_pkDb || !m_pkDb->Execute(kResults, szSql))
		return false;
	if(!kResults.Step())
		return false;
	const char* sz = kResults.GetText(0);
	strOut = sz ? sz : "";
	return true;
}

std::string HstDatabase::Lookup(const char* szTextKey)
{
	if(!m_bLocalization)
		return std::string();
	Localization::String kString = Localization::Lookup(szTextKey);
	const char* sz = kString.toUTF8();
	return sz ? std::string(sz) : std::string();
}

bool HstDatabase::ParseGuid(const char* szHex, GUID& kOut)
{
	unsigned char aucNibbles[32];
	int iCount = 0;
	for(const char* p = szHex; *p && iCount < 32; ++p)
	{
		char c = *p;
		if(c >= '0' && c <= '9')
			aucNibbles[iCount++] = (unsigned char)(c - '0');
		else if(c >= 'a' && c <= 'f')
			aucNibbles[iCount++] = (unsigned char)(c - 'a' + 10);
		else if(c >= 'A' && c <= 'F')
			aucNibbles[iCount++] = (unsigned char)(c - 'A' + 10);
	}
	if(iCount != 32)
		return false;
	unsigned int auiBytes[16];
	for(int i = 0; i < 16; ++i)
		auiBytes[i] = (aucNibbles[2 * i] << 4) | aucNibbles[2 * i + 1];
	kOut.Data1 = (auiBytes[0] << 24) | (auiBytes[1] << 16) | (auiBytes[2] << 8) | auiBytes[3];
	kOut.Data2 = (unsigned short)((auiBytes[4] << 8) | auiBytes[5]);
	kOut.Data3 = (unsigned short)((auiBytes[6] << 8) | auiBytes[7]);
	for(int i = 0; i < 8; ++i)
		kOut.Data4[i] = (unsigned char)auiBytes[8 + i];
	return true;
}

void HstDatabase::GetActiveDlc(std::vector<GUID>& aOut)
{
	Database::Results kResults;
	if(!m_pkDb || !m_pkDb->Execute(kResults, "SELECT PackageID FROM DownloadableContent WHERE IsActive = 1 ORDER BY rowid"))
		return;
	while(kResults.Step())
	{
		GUID kGuid;
		const char* sz = kResults.GetText(0);
		if(sz && ParseGuid(sz, kGuid))
			aOut.push_back(kGuid);
	}
}
