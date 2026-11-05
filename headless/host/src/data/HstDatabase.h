//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstDatabase.h
//!  \brief     Gameplay database + localization (design 1.3, B1-B4) on the game's database/localization DLLs.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTDATABASE_H
#define HSTDATABASE_H

#include "HstCommon.h"

class HstDatabase
{
public:
	HstDatabase();
	~HstDatabase();

	//! B1-B2: open ":memory:" and BinaryIO::Load the gameplay DB file into it (the file is never written).
	bool OpenGameplay(const std::string& strUtf8GameplayDb);
	//! B3: ATTACH the localization DB as "Localization".
	bool AttachLocalization(const std::string& strUtf8LocalizationDb);
	//! B4: Localization::Initialize(icu dat, db, "Languages", language, false).
	bool InitLocalization(const std::wstring& wstrIcuDat, const char* szLanguage);

	Database::Connection* GetConnection() const { return m_pkDb; }
	int Count(const char* szTable);
	//! Single-value SQL query helper; returns false when no row.
	bool QueryText(const char* szSql, std::string& strOut);
	std::string Lookup(const char* szTextKey);
	//! GUIDs of the active rows of DownloadableContent (I8).
	void GetActiveDlc(std::vector<GUID>& aOut);

	static bool ParseGuid(const char* szHex32, GUID& kOut);

protected:
	Database::Connection* m_pkDb;
	bool m_bLocalization;
};

#endif // HSTDATABASE_H
