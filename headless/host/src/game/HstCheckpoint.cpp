//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstCheckpoint.cpp
//!  \brief     Checkpoint file I/O (format in HstCheckpoint.h). The load/save sequences live in HstSession.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "game/HstCheckpoint.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"

void HstCheckpointFile::Add(const std::string& strName, const void* pData, unsigned int uiBytes)
{
	m_akSections.push_back(Section());
	m_akSections.back().strName = strName;
	if(uiBytes)
		m_akSections.back().strData.assign((const char*)pData, uiBytes);
}

const HstCheckpointFile::Section* HstCheckpointFile::Find(const std::string& strName) const
{
	for(size_t i = 0; i < m_akSections.size(); ++i)
		if(m_akSections[i].strName == strName)
			return &m_akSections[i];
	return NULL;
}

std::string HstCheckpointFile::HeaderValue(const std::string& strHeader, const char* szKey)
{
	const std::string strKey = std::string(szKey) + "=";
	size_t uiPos = 0;
	while(uiPos < strHeader.size())
	{
		size_t uiEnd = strHeader.find('\n', uiPos);
		if(uiEnd == std::string::npos)
			uiEnd = strHeader.size();
		if(strHeader.compare(uiPos, strKey.size(), strKey) == 0)
			return strHeader.substr(uiPos + strKey.size(), uiEnd - uiPos - strKey.size());
		uiPos = uiEnd + 1;
	}
	return std::string();
}

bool HstCheckpointFile::Write(const std::wstring& wstrPath, unsigned int& uiBytes, HstHash64& ui64Hash, std::string& strError) const
{
	std::string strOut;
	strOut.append("CHCK", 4);
	const unsigned int uiVersion = 1;
	const unsigned int uiCount = (unsigned int)m_akSections.size();
	strOut.append((const char*)&uiVersion, 4);
	strOut.append((const char*)&uiCount, 4);
	for(size_t i = 0; i < m_akSections.size(); ++i)
	{
		const Section& k = m_akSections[i];
		const unsigned short usLen = (unsigned short)k.strName.size();
		const unsigned int uiSize = (unsigned int)k.strData.size();
		const HstHash64 ui64 = HstXxh64(k.strData.data(), k.strData.size());
		strOut.append((const char*)&usLen, 2);
		strOut.append(k.strName);
		strOut.append((const char*)&uiSize, 4);
		strOut.append((const char*)&ui64, 8);
		strOut.append(k.strData);
	}
	FILE* pkFile = _wfopen(wstrPath.c_str(), L"wb");
	if(!pkFile)
	{
		strError = "cannot create " + HstToUtf8(wstrPath);
		return false;
	}
	const size_t uiWritten = fwrite(strOut.data(), 1, strOut.size(), pkFile);
	fclose(pkFile);
	if(uiWritten != strOut.size())
	{
		strError = "short write to " + HstToUtf8(wstrPath);
		return false;
	}
	uiBytes = (unsigned int)strOut.size();
	ui64Hash = HstXxh64(strOut.data(), strOut.size());
	return true;
}

bool HstCheckpointFile::Read(const std::wstring& wstrPath, std::string& strError)
{
	m_akSections.clear();
	FILE* pkFile = _wfopen(wstrPath.c_str(), L"rb");
	if(!pkFile)
	{
		strError = "cannot open " + HstToUtf8(wstrPath);
		return false;
	}
	fseek(pkFile, 0, SEEK_END);
	const long lSize = ftell(pkFile);
	fseek(pkFile, 0, SEEK_SET);
	std::string strIn;
	strIn.resize(lSize > 0 ? lSize : 0);
	const size_t uiRead = lSize > 0 ? fread(&strIn[0], 1, lSize, pkFile) : 0;
	fclose(pkFile);
	if((long)uiRead != lSize || strIn.size() < 12 || strIn.compare(0, 4, "CHCK") != 0)
	{
		strError = "not a civhost checkpoint: " + HstToUtf8(wstrPath);
		return false;
	}
	unsigned int uiVersion = 0, uiCount = 0;
	memcpy(&uiVersion, &strIn[4], 4);
	memcpy(&uiCount, &strIn[8], 4);
	if(uiVersion != 1)
	{
		strError = "unsupported checkpoint version";
		return false;
	}
	size_t uiPos = 12;
	for(unsigned int i = 0; i < uiCount; ++i)
	{
		if(uiPos + 2 > strIn.size())
			break;
		unsigned short usLen = 0;
		memcpy(&usLen, &strIn[uiPos], 2);
		uiPos += 2;
		if(uiPos + usLen + 12 > strIn.size())
			break;
		Section kSection;
		kSection.strName = strIn.substr(uiPos, usLen);
		uiPos += usLen;
		unsigned int uiSize = 0;
		HstHash64 ui64 = 0;
		memcpy(&uiSize, &strIn[uiPos], 4);
		memcpy(&ui64, &strIn[uiPos + 4], 8);
		uiPos += 12;
		if(uiPos + uiSize > strIn.size())
			break;
		kSection.strData = strIn.substr(uiPos, uiSize);
		uiPos += uiSize;
		if(HstXxh64(kSection.strData.data(), kSection.strData.size()) != ui64)
		{
			strError = "checkpoint section '" + kSection.strName + "' is corrupt (hash mismatch)";
			return false;
		}
		m_akSections.push_back(kSection);
	}
	if(m_akSections.size() != uiCount)
	{
		strError = "checkpoint is truncated";
		return false;
	}
	return true;
}
