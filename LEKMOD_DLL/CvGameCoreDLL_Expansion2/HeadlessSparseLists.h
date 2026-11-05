#pragma once
// Headless platform, OPT-0041 (LEKMOD_HEADLESS_OPT_DB_SPARSE_LISTS): per-key ascending id lists of the database entries
// (policies, buildings) whose contribution to a per-entry sum can be nonzero for that key. Golden code loops over every
// entry id and adds entry->Get(key); entries whose getter returns 0 for the key add nothing, so visiting only the listed ids
// (ascending, i.e. golden's relative order) with golden's unchanged per-entry tests gives the same sum and the same calls
// with nonzero effect. The lists are a pure function of the loaded database entries, which do not change during a game;
// they are rebuilt whenever g_uiHeadlessDbStaticEpoch moves (every (re)load / write of the game database) or the entry or
// key count differs. Never keyed or ordered by pointer.
#if defined(LEKMOD_HEADLESS_OPT_DB_SPARSE_LISTS)

#include <vector>

extern unsigned int g_uiHeadlessDbStaticEpoch;	// CvGlobals.cpp

class CvHeadlessSparseLists
{
public:
	CvHeadlessSparseLists() : m_uiEpoch(0), m_iNumEntries(-1), m_iNumKeys(-1) {}

	bool IsValid(int iNumEntries, int iNumKeys) const
	{
		return m_uiEpoch == g_uiHeadlessDbStaticEpoch && m_iNumEntries == iNumEntries && m_iNumKeys == iNumKeys;
	}

	// kRelevant(iEntry, iKey) -> true if the entry's contribution for the key can be nonzero (must be a pure DB read).
	template<class Rel>
	void Build(int iNumEntries, int iNumKeys, const Rel& kRelevant)
	{
		m_aiStart.assign(iNumKeys + 1, 0);
		m_aiIds.clear();
		for(int iKey = 0; iKey < iNumKeys; iKey++)
		{
			m_aiStart[iKey] = (int)m_aiIds.size();
			for(int iEntry = 0; iEntry < iNumEntries; iEntry++)
			{
				if(kRelevant(iEntry, iKey))
					m_aiIds.push_back(iEntry);
			}
		}
		m_aiStart[iNumKeys] = (int)m_aiIds.size();
		m_uiEpoch = g_uiHeadlessDbStaticEpoch;
		m_iNumEntries = iNumEntries;
		m_iNumKeys = iNumKeys;
	}

	int Count(int iKey) const { return m_aiStart[iKey + 1] - m_aiStart[iKey]; }
	int Id(int iKey, int iPos) const { return m_aiIds[m_aiStart[iKey] + iPos]; }

private:
	unsigned int m_uiEpoch;
	int m_iNumEntries;
	int m_iNumKeys;
	std::vector<int> m_aiStart;
	std::vector<int> m_aiIds;
};

#endif
