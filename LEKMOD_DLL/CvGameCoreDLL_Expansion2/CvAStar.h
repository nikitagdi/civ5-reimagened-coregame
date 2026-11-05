/*	-------------------------------------------------------------------------------------------------------
	� 1991-2012 Take-Two Interactive Software and its subsidiaries.  Developed by Firaxis Games.  
	Sid Meier's Civilization V, Civ, Civilization, 2K Games, Firaxis Games, Take-Two Interactive Software 
	and their respective logos are all trademarks of Take-Two interactive Software, Inc.  
	All other marks and trademarks are the property of their respective owners.  
	All rights reserved. 
	------------------------------------------------------------------------------------------------------- */
#pragma once

//
//  AUTHOR:  Casey O'Toole  --  8/21/2002
//
//  PURPOSE: A* Pathfinding - based off of A* Explorer from "AI Game Programming Wisdom"
//

#ifndef		CVASTAR_H
#define		CVASTAR_H
#pragma		once

#include	"CvAStarNode.h"

// headless OPT-0012/OPT-0013/OPT-0015 share the per-search epoch of CvAStar (see HeadlessInSearch below)
#if (defined(LEKMOD_HEADLESS_OPT_ASTAR_MOVE_MEMO) || defined(LEKMOD_HEADLESS_OPT_ASTAR_NODE_CACHE_MEMO) || defined(LEKMOD_HEADLESS_OPT_ASTAR_WALK_MEMO) || defined(LEKMOD_HEADLESS_OPT_ASTAR_FROM_PLOT_MEMO)) && !defined(LEKMOD_HEADLESS_ASTAR_SEARCH_EPOCH)
#define LEKMOD_HEADLESS_ASTAR_SEARCH_EPOCH
#endif
// headless OPT-0090: early stop of TurnsToReachTarget searches that cannot reach their destination (the study build uses the
// same hook to measure without stopping)
#if (defined(LEKMOD_HEADLESS_OPT_ASTAR_UNREACHABLE_STOP) || defined(LEKMOD_STUDY_FAILPATH_STATS)) && !defined(LEKMOD_HEADLESS_ASTAR_STOP_HOOK)
#define LEKMOD_HEADLESS_ASTAR_STOP_HOOK
#endif
#if defined(LEKMOD_HEADLESS_ASTAR_STOP_HOOK) && !defined(HEADLESS_STOP_CHECK_EVERY)
#define HEADLESS_STOP_CHECK_EVERY 32		// OPT-0090: pops between two stop checks of a search (the turn bound test)
#define HEADLESS_STOP_CLOSURE_EVERY 128	// OPT-0090: pops between two unreachability walks (a multiple of HEADLESS_STOP_CHECK_EVERY)
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_WALK_MEMO) && !defined(LEKMOD_HEADLESS_OPT_ASTAR_NODE_CACHE_MEMO)
#error OPT-0015 (LEKMOD_HEADLESS_OPT_ASTAR_WALK_MEMO) relies on OPT-0013 (LEKMOD_HEADLESS_OPT_ASTAR_NODE_CACHE_MEMO): the node cost cache must change only at its first fill per search
#endif

#if defined(LEKMOD_HEADLESS_OPT_ASTAR_OPEN_BUCKETS)
#if defined(AUI_ASTAR_FIX_POSSIBLE_NULL_POINTERS) || defined(AUI_ASTAR_MINOR_OPTIMIZATION)
#error OPT-0086 (LEKMOD_HEADLESS_OPT_ASTAR_OPEN_BUCKETS) reproduces the list order of the build without AUI_ASTAR_FIX_POSSIBLE_NULL_POINTERS / AUI_ASTAR_MINOR_OPTIMIZATION only
#endif
#include <vector>
// headless OPT-0086: one run of equal total costs in the sorted open list (first and last node of the run)
struct CvAStarOpenBucket
{
	int iCost;
	CvAStarNode* pFirst;
	CvAStarNode* pLast;
};
#endif

class CvAStar;
#if defined(LEKMOD_HEADLESS_FIX_0020)
class CvIgnoreUnitsPathFinder;
#endif

#ifdef AUI_CONSTIFY
typedef int(*CvAPointFunc)(int, int, const void*, const CvAStar*);
#else
typedef int(*CvAPointFunc)(int, int, const void*, CvAStar*);
#endif
typedef int(*CvAHeuristic)(int, int, int, int);
typedef int(*CvAStarFunc)(CvAStarNode*, CvAStarNode*, int, const void*, CvAStar*);
typedef int(*CvANumExtraChildren)(CvAStarNode*, CvAStar*);
typedef int(*CvAGetExtraChild)(CvAStarNode*, int, int&, int&, CvAStar*);
typedef void(*CvABegin)(const void*, CvAStar*);
typedef void(*CvAEnd)(const void*, CvAStar*);

// PATHFINDER FLAGS
// WARNING: Some of these flags are passed into the unit mission and stored in the missions iFlags member.
//          Because the mission's iFlags are sharing space with the pathfinder flags, we currently have mission
//			modifier flags listed below that really don't have anything to do with the pathfinder.
//			A fix for this would be to have the mission contain separate pathfinder and modifier flags.
// These flags determine plots that can be entered
#define MOVE_TERRITORY_NO_UNEXPLORED		(0x00000001)
#define MOVE_TERRITORY_NO_ENEMY				(0x00000002)
#define MOVE_IGNORE_STACKING                (0x00000004)
// These two tell about presence of enemy units
#define MOVE_UNITS_IGNORE_DANGER			(0x00000008)
#define MOVE_UNITS_THROUGH_ENEMY			(0x00000010)
// Used for human player movement
#define MOVE_DECLARE_WAR					(0x00000020)
// Used for AI group attacks (??). Not really a pathfinder flag
#define MISSION_MODIFIER_DIRECT_ATTACK		(0x00000040)
#define MISSION_MODIFIER_NO_DEFENSIVE_SUPPORT (0x00000100)

#define MOVE_MAXIMIZE_EXPLORE				(0x00000080)
//
// Used for route information
#define MOVE_ANY_ROUTE					    (0x80000000) // because we're passing in the player number as well as the route flag
#define MOVE_ROUTE_ALLOW_UNEXPLORED			(0x40000000) // When searching for a route, allow the search to use unrevealed plots
//#define MOVE_NON_WAR_ROUTE				 // we're passing the player id and other flags in as well. This flag checks to see if it can get from point to point without going into territory with a team we're at war with

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//
//  CLASS:      CvAStar
//
//  DESC:       CvAStar pathfinding class
//
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
class CvAStar
{

	//--------------------------------------- PUBLIC METHODS -------------------------------------------
public:
	enum RANGES
	{
		SCRATCH_BUFFER_SIZE = 512
	};
	// Constructor
	CvAStar();

	// Destructor
	~CvAStar();

	// Initializes the CvAStar class. iSize = Dimensions of Pathing Grid(ie. [iSize][iSize]
	void Initialize(int iColumns, int iRows, bool bWrapX, bool bWrapY, CvAPointFunc IsPathDestFunc, CvAPointFunc DestValidFunc, CvAHeuristic HeuristicFunc, CvAStarFunc CostFunc, CvAStarFunc ValidFunc, CvAStarFunc NotifyChildFunc, CvAStarFunc NotifyListFunc, CvANumExtraChildren NumExtraChildrenFunc, CvAGetExtraChild GetExtraChildFunc, CvABegin InitializeFunc, CvAEnd UninitializeFunc, const void* pData);

	void DeInit();		// free memory

	// Generates a path
	bool GeneratePath(int iXstart, int iYstart, int iXdest, int iYdest, int iInfo = 0, bool bReuse = false);

	// Gets the last node in the path (from the origin) - Traverse the parents to get full path (linked list starts at destination)
#ifdef AUI_CONSTIFY
	inline CvAStarNode* GetLastNode() const
#else
	inline CvAStarNode* GetLastNode()
#endif
	{
		return m_pBest;
	}

#ifdef AUI_ASTAR_GET_PENULTIMATE_NODE
	// Gets the node before the last node in the path (from the origin)
	inline CvAStarNode* GetPenultimateNode() const
	{
		return (m_pBest ? m_pBest->m_pParent : NULL);
}
#endif

#ifdef AUI_CONSTIFY
	inline bool IsPathStart(int iX, int iY) const
#else
	inline bool IsPathStart(int iX, int iY)
#endif
	{
		return ((m_iXstart == iX) && (m_iYstart == iY));
	}

#ifdef AUI_CONSTIFY
	inline bool IsPathDest(int iX, int iY) const
#else
	inline bool IsPathDest(int iX, int iY)
#endif
	{
		if(udIsPathDest && udIsPathDest(iX, iY, m_pData, this))
		{
			return TRUE;
		}
		return FALSE;
	}

#ifdef AUI_CONSTIFY
	inline int GetStartX() const
#else
	inline int GetStartX()
#endif
	{
		return m_iXstart;
	}

#ifdef AUI_CONSTIFY
	inline int GetStartY() const
#else
	inline int GetStartY()
#endif
	{
		return m_iYstart;
	}

#ifdef AUI_CONSTIFY
	inline int GetDestX() const
#else
	inline int GetDestX()
#endif
	{
		return m_iXdest;
	}

#ifdef AUI_CONSTIFY
	inline int GetDestY() const
#else
	inline int GetDestY()
#endif
	{
		return m_iYdest;
	}

#ifdef AUI_CONSTIFY
	inline int GetInfo() const
#else
	inline int GetInfo()
#endif
	{
		return m_iInfo;
	}

	inline void ForceReset()
	{
		m_bForceReset = true;
	}

#ifdef AUI_ASTAR_TURN_LIMITER
	inline int GetMaxTurns() const
	{
		return m_iMaxTurns;
	}

	inline void SetMaxTurns(int iMaxTurns)
	{
		if (m_bDataChangeInvalidatesCache && m_iMaxTurns != iMaxTurns)
			m_bForceReset = true;
		m_iMaxTurns = iMaxTurns;
	}
#endif

#ifdef AUI_ASTAR_TURN_LIMITER
	inline void SetData(const void* pData, int iMaxTurns = MAX_INT)
	{
		if (m_bDataChangeInvalidatesCache && (m_pData != pData || m_iMaxTurns != iMaxTurns))
#else
	inline void SetData(const void* pData)
	{
		if(m_bDataChangeInvalidatesCache && m_pData != pData)
#endif
			m_bForceReset = true;
		m_pData = pData;
#ifdef AUI_ASTAR_TURN_LIMITER
		m_iMaxTurns = iMaxTurns;
#endif
	}

	inline bool IsMPCacheSafe() const
	{
		return m_bIsMPCacheSafe;
	}

	inline bool SetMPCacheSafe(bool bState)
	{
		bool bOldState = m_bIsMPCacheSafe;
		if(bState != m_bIsMPCacheSafe)
		{
			m_bForceReset = true;
			m_bIsMPCacheSafe = bState;
		}

		return bOldState;
	}

	inline bool GetDataChangeInvalidatesCache() const
	{
		return m_bDataChangeInvalidatesCache;
	}

	inline bool SetDataChangeInvalidatesCache(bool bState)
	{
		bool bOldState = m_bDataChangeInvalidatesCache;
		m_bDataChangeInvalidatesCache = bState;

		return bOldState;
	}

#ifdef AUI_CONSTIFY
	inline CvAPointFunc GetIsPathDestFunc() const
#else
	inline CvAPointFunc GetIsPathDestFunc()
#endif
	{
		return udIsPathDest;
	}

	inline void SetIsPathDestFunc(CvAPointFunc newIsPathDestFunc)
	{
		udIsPathDest = newIsPathDestFunc;
	}

#ifdef AUI_CONSTIFY
	inline CvAPointFunc GetDestValidFunc() const
#else
	inline CvAPointFunc GetDestValidFunc()
#endif
	{
		return udDestValid;
	}

	inline void SetDestValidFunc(CvAPointFunc newDestValidFunc)
	{
		udDestValid = newDestValidFunc;
	}

#ifdef AUI_CONSTIFY
	inline CvAHeuristic GetHeuristicFunc() const
#else
	inline CvAHeuristic GetHeuristicFunc()
#endif
	{
		return udHeuristic;
	}

	inline void SetHeuristicFunc(CvAHeuristic newHeuristicFunc)
	{
		udHeuristic = newHeuristicFunc;
	}

#if defined(AUI_CONSTIFY)
	inline CvAStarFunc GetCostFunc() const
#else
	inline CvAStarFunc GetCostFunc()
#endif
	{
		return udCost;
	}

	inline void SetCostFunc(CvAStarFunc newCostFunc)
	{
		udCost = newCostFunc;
	}

#ifdef AUI_CONSTIFY
	inline CvAStarFunc GetValidFunc() const
#else
	inline CvAStarFunc GetValidFunc()
#endif
	{
		return udValid;
	}

	inline void SetValidFunc(CvAStarFunc newValidFunc)
	{
		udValid = newValidFunc;
	}

#if defined(AUI_CONSTIFY)
	inline CvAStarFunc GetNotifyChildFunc() const
#else
	inline CvAStarFunc GetNotifyChildFunc()
#endif
	{
		return udNotifyChild;
	}

	inline void SetNotifyChildFunc(CvAStarFunc newNotifyChildFunc)
	{
		udNotifyChild = newNotifyChildFunc;
	}

#if defined(AUI_CONSTIFY)
	inline CvAStarFunc GetNotifyListFunc() const
#else
	inline CvAStarFunc GetNotifyListFunc()
#endif
	{
		return udNotifyList;
	}

	inline void SetNotifyListFunc(CvAStarFunc newNotifyListFunc)
	{
		udNotifyList = newNotifyListFunc;
	}

#ifdef AUI_CONSTIFY
	inline CvANumExtraChildren GetNumExtraChildrenFunc() const
#else
	inline CvANumExtraChildren GetNumExtraChildrenFunc()
#endif
	{
		return udNumExtraChildrenFunc;
	}

	inline void SetNumExtraChildrenFunc(CvANumExtraChildren newNumExtraChildrenFunc)
	{
		udNumExtraChildrenFunc = newNumExtraChildrenFunc;
	}

#ifdef AUI_CONSTIFY
	inline CvAGetExtraChild GetExtraChildGetterFunc() const
#else
	inline CvAGetExtraChild GetExtraChildGetterFunc()
#endif
	{
		return udGetExtraChildFunc;
	}

	inline void SetExtraChildGetterFunc(CvAGetExtraChild newGetExtraChildFunc)
	{
		udGetExtraChildFunc = newGetExtraChildFunc;
	}

	void AddToOpen(CvAStarNode* addnode);

	// Copy the supplied node and its parent nodes into an array of simpler path nodes for caching purposes.
	// It is ok to pass in NULL, the resulting array will contain zero elements
	static void CopyPath(const CvAStarNode* pkEndNode, CvPathNodeArray& kPathArray);

#ifndef AUI_ASTAR_MINOR_OPTIMIZATION
	void* GetScratchPointer1() { return m_pScratchPtr1; }
	void  SetScratchPointer1(void* pPtr) { m_pScratchPtr1 = pPtr; }
	void* GetScratchPointer2() { return m_pScratchPtr2; }
	void  SetScratchPointer2(void* pPtr) { m_pScratchPtr1 = pPtr; }
#endif

#ifdef AUI_CONSTIFY
	const void* CvAStar::GetScratchBuffer() const { return &m_ScratchBuffer[0]; }
#endif
	void* GetScratchBuffer() { return &m_ScratchBuffer[0]; }

#if defined(LEKMOD_HEADLESS_ASTAR_SEARCH_EPOCH)
	// headless OPT-0012/OPT-0013: every GeneratePath call is one "search" with its own epoch number. Inside one search the
	// unit (m_pData), the flags and the game state are constant (the callbacks only read), so values the callbacks derive
	// from them can be memoised for the rest of that search and are void when the next search starts.
	inline bool HeadlessInSearch() const { return m_bHeadlessInSearch; }
	inline int HeadlessSearchEpoch() const { return m_iHeadlessSearchEpoch; }
	inline int HeadlessNumPlots() const { return m_iColumns * m_iRows; }
	inline int HeadlessColumns() const { return m_iColumns; }
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_MOVE_MEMO)
	// OPT-0012: per-search memo of the pure movement predicates of PathCost/PathAdd (CvAStar.cpp, "OPT-0012").
	struct HeadlessZocMemo
	{
		int iEpoch;					// search epoch the masks belong to (0 = never computed)
		unsigned char ucCityMask;	// bit d: plotDirection(from, d) is an enemy city that exerts ZOC on this unit
		unsigned char ucUnitMask;	// bit d: plotDirection(from, d) holds an enemy combat unit that exerts ZOC on this unit
	};
	struct HeadlessMoveMemo
	{
		int iEpoch;					// search epoch of the entry (0 = empty)
		const CvPlot* pFrom;
		const CvPlot* pTo;
		signed char cConsumesAllMoves;	// -1 not computed yet, else 0/1
		signed char cCostsOnlyOne;
		signed char cSlowedByZOC;
		bool bTerrainCostValid;
		int iTerrainCostBaseMoves;	// iBaseMoves the terrain cost below was computed with
		int iTerrainCost;			// max(1, min(regular, route, flat route)) of CvUnitMovement::GetCostsForMove
	};
	inline HeadlessZocMemo* HeadlessZocMemoAt(int iPlotIndex) { return &m_paHeadlessZocMemo[iPlotIndex]; }
	inline HeadlessMoveMemo& HeadlessMoveMemoEntry() { return m_kHeadlessMoveMemo; }
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_WALK_MEMO)
	// OPT-0015: version of the search tree. A node's memoised walk result is valid while its version equals this one.
	// Bumped at every search start and whenever a CLOSED node's parent, turn count (m_iData2) or cost cache changes (only
	// closed nodes have descendants and memos; see CvAStar.cpp, "OPT-0015").
	inline int HeadlessTreeVersion() const { return m_iHeadlessTreeVersion; }
	inline void HeadlessBumpTreeVersion() { ++m_iHeadlessTreeVersion; }
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_FROM_PLOT_MEMO)
	// OPT-0017: per-search memo of the from-plot tests at the end of PathValid/TacticalAnalysisMapPathValid
	struct HeadlessFromMemo
	{
		int iEpoch;						// search epoch (0 = never)
		signed char cMoveThrough;		// pUnit->canMoveThrough(from, MOVEFLAG_PRETEND_CORRECT_EMBARK_STATE): -1 unknown, 0/1
		signed char cMoveOrAttackInto;	// pUnit->canMoveOrAttackInto(from, MOVEFLAG_PRETEND_CORRECT_EMBARK_STATE): -1 unknown, 0/1
	};
	inline HeadlessFromMemo* HeadlessFromMemoAt(int iPlotIndex) { return &m_paHeadlessFromMemo[iPlotIndex]; }
#endif
#if defined(LEKMOD_HEADLESS_ASTAR_STOP_HOOK)
	// OPT-0090: early stop of a search that can no longer reach its destination (CvAStar.cpp, "OPT-0090"). While a stop check
	// is set, GeneratePath asks it every so many pops; when it answers true the search returns false at once (the full search
	// returns false too, or with a turn bound reaches the target only beyond it) and the finder remembers the rest was not run. HeadlessFinishStoppedSearch runs that
	// rest (the game state must be the one of the stopped search); GeneratePath does so itself before a search that reuses the
	// stopped tree (bReuse with the same start and flags), the only way the rest could be observed.
	typedef bool (*HeadlessStopCheck)(CvAStar* pFinder, int iSteps);
	inline void HeadlessSetStopCheck(HeadlessStopCheck pfnCheck) { m_pfnHeadlessStopCheck = pfnCheck; }
	inline bool HeadlessStoppedEarly() const { return m_bHeadlessStoppedEarly; }
	bool HeadlessFinishStoppedSearch();	// returns what the full GeneratePath would have returned (false if none stopped)
	inline const CvAStarNode* HeadlessMainNode(int iX, int iY) const { return &m_ppaaNodes[iX][iY]; }
	inline const void* HeadlessData() const { return m_pData; }
	int HeadlessMinOpenTurns() const;	// smallest m_iData2 of the open nodes (MAX_INT if none); the one reader of the open list
#endif
#if defined(LEKMOD_HEADLESS_FIX_0020)
	// FIX-0020 / DS-0035: private finder for path queries made for the local UI only (see HeadlessUiIgnoreUnitsPathFinder).
	// Set only on GC.getIgnoreUnitsPathFinder(): created with the same grid and callbacks in every Initialize, freed in DeInit.
	inline CvIgnoreUnitsPathFinder* HeadlessUiTwin() const { return m_pHeadlessUiTwin; }
#endif
	//--------------------------------------- PROTECTED FUNCTIONS -------------------------------------------
protected:

	int     Step();
	void    Reset()
	{
		m_pBest = NULL;
	}

	CvAStarNode*	GetBest();

#ifdef AUI_ASTAR_PRECALCULATE_NEIGHBORS_ON_INITIALIZE
	void PrecalcNeighbors(CvAStarNode* node); // headless OPT-0002: not const, xRange/yRange/isValid are const only with AUI_CONSTIFY
#endif
	void CreateChildren(CvAStarNode* node);
	void LinkChild(CvAStarNode* node, CvAStarNode* check);
	void UpdateOpenNode(CvAStarNode* node);
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_OPEN_BUCKETS)
	// headless OPT-0086: index of the open list by cost (the list itself, m_pOpen / m_pNext / m_pPrev, is unchanged)
	void OpenIndexReset();
	size_t OpenIndexLower(int iCost) const;
	size_t OpenIndexUpper(int iCost) const;
	void OpenIndexInsertAt(size_t iAt, const CvAStarOpenBucket& kBucket);
	void OpenIndexPushFront(CvAStarNode* node, int iCost);
	void OpenIndexPushBack(CvAStarNode* node, int iCost);
	void OpenIndexRemove(CvAStarNode* node);
	void OpenIndexPopHead(CvAStarNode* node);
	CvAStarNode* OpenIndexLastNotAbove(int iCost) const;
	CvAStarNode* OpenIndexFirstNotBelow(int iCost) const;
#if defined(LEKMOD_HEADLESS_OPT_VERIFY)
	void OpenVerifyAll(const char* pszWhere);
#endif
#endif
	void UpdateParents(CvAStarNode* node);

	void Push(CvAStarNode* node);
	CvAStarNode* Pop();

#ifdef AUI_CONSTIFY
	inline int xRange(int iX) const;
	inline int yRange(int iY) const;
	inline bool isValid(int iX, int iY) const;
#else
	inline int xRange(int iX);
	inline int yRange(int iY);
	inline bool isValid(int iX, int iY);
#endif

	inline int udFunc(CvAStarFunc func, CvAStarNode* param1, CvAStarNode* param2, int data, const void* cb);

	//--------------------------------------- PROTECTED DATA -------------------------------------------
protected:
	CvAPointFunc udIsPathDest;					// Determines if this node is the destination of the path
	CvAPointFunc udDestValid;				    // Determines destination is valid
	CvAHeuristic udHeuristic;				    // Determines heuristic cost
	CvAStarFunc udCost;						    // Called when cost value is need
	CvAStarFunc udValid;					    // Called to check validity of a coordinate
	CvAStarFunc udNotifyChild;				    // Called when child is added/checked (LinkChild)
	CvAStarFunc udNotifyList;				    // Called when node is added to Open/Closed list
	CvANumExtraChildren udNumExtraChildrenFunc; // Determines if CreateChildren should consider any additional nodes
	CvAGetExtraChild udGetExtraChildFunc;	    // Get the extra children nodes
	CvABegin udInitializeFunc;					// Called at the start, to initialize any run specific data
	CvAEnd udUninitializeFunc;					// Called at the end to uninitialize any run specific data


	const void* m_pData;			// Data passed back to functions

#ifdef AUI_ASTAR_TURN_LIMITER
	int m_iMaxTurns;				// Pathfinder never lets a path's turn cost become higher than this number
#endif

	int m_iColumns;					// Used to calculate node->number
	int m_iRows;					// Used to calculate node->number
	int m_iXstart;
	int m_iYstart;
	int m_iXdest;
	int m_iYdest;
	int m_iInfo;

	bool m_bWrapX;
	bool m_bWrapY;
	bool m_bForceReset;
	bool m_bIsMPCacheSafe;
	bool m_bDataChangeInvalidatesCache;
#ifdef AUI_ASTAR_MINOR_OPTIMIZATION
	bool m_bIsMultiplayer;
#endif

	CvAStarNode* m_pOpen;            // The open list
	CvAStarNode* m_pOpenTail;        // The open list tail pointer (to speed up inserts)
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_OPEN_BUCKETS)
	std::vector<CvAStarOpenBucket> m_aOpenBuckets;	// headless OPT-0086: the runs of equal cost of the open list, ascending; live ones are [m_iOpenBucketsHead, size())
	size_t m_iOpenBucketsHead;
#endif
	CvAStarNode* m_pClosed;          // The closed list
	CvAStarNode* m_pBest;            // The best node
	CvAStarNode* m_pStackHead;		// The Push/Pop stack head

	CvAStarNode** m_ppaaNodes;

#ifndef AUI_ASTAR_MINOR_OPTIMIZATION
	// Scratch buffers
	void* m_pScratchPtr1;						// Will be cleared to NULL before each GeneratePath call
	void* m_pScratchPtr2;						// Will be cleared to NULL before each GeneratePath call
#endif

	char  m_ScratchBuffer[SCRATCH_BUFFER_SIZE];	// Will NOT be modified directly by CvAStar

#if defined(LEKMOD_HEADLESS_ASTAR_SEARCH_EPOCH)
	int m_iHeadlessSearchEpoch;		// headless OPT-0012/0013: incremented at the start of every GeneratePath
	bool m_bHeadlessInSearch;		// true while GeneratePath runs (memos are only used by callbacks called from it)
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_MOVE_MEMO)
	HeadlessZocMemo* m_paHeadlessZocMemo;	// OPT-0012: [m_iColumns * m_iRows], index y * m_iColumns + x of the from-plot
	HeadlessMoveMemo m_kHeadlessMoveMemo;	// OPT-0012: last (from, to) edge evaluated in this search
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_WALK_MEMO)
	int m_iHeadlessTreeVersion;				// OPT-0015
#endif
#if defined(LEKMOD_HEADLESS_OPT_ASTAR_FROM_PLOT_MEMO)
	HeadlessFromMemo* m_paHeadlessFromMemo;	// OPT-0017: [m_iColumns * m_iRows], index y * m_iColumns + x of the from-plot
#endif
#if defined(LEKMOD_HEADLESS_FIX_0020)
	CvIgnoreUnitsPathFinder* m_pHeadlessUiTwin;	// FIX-0020: UI-only copy of GC.getIgnoreUnitsPathFinder() (NULL on every other finder)
#endif
#if defined(LEKMOD_HEADLESS_ASTAR_STOP_HOOK)
	HeadlessStopCheck m_pfnHeadlessStopCheck;	// OPT-0090: set by TurnsToReachTarget for one GeneratePath call, NULL otherwise
	bool m_bHeadlessStoppedEarly;				// OPT-0090: the last search stopped early, the rest of it was not run
#endif
};


#ifdef AUI_CONSTIFY
inline int CvAStar::xRange(int iX) const
#else
inline int CvAStar::xRange(int iX)
#endif
{
	if(m_bWrapX)
	{
		if(iX < 0)
		{
			return (m_iColumns + (iX % m_iColumns));
		}
		else if(iX >= m_iColumns)
		{
			return (iX % m_iColumns);
		}
		else
		{
			return iX;
		}
	}
	else
	{
		return iX;
	}
}


#ifdef AUI_CONSTIFY
inline int CvAStar::yRange(int iY) const
#else
inline int CvAStar::yRange(int iY)
#endif
{
	if(m_bWrapY)
	{
		if(iY < 0)
		{
			return (m_iRows + (iY % m_iRows));
		}
		else if(iY >= m_iRows)
		{
			return (iY % m_iRows);
		}
		else
		{
			return iY;
		}
	}
	else
	{
		return iY;
	}
}


#ifdef AUI_CONSTIFY
inline bool CvAStar::isValid(int iX, int iY) const
#else
inline bool CvAStar::isValid(int iX, int iY)
#endif
{
	if((iX < 0) || (iX >= m_iColumns))
	{
		return false;
	}

	if((iY < 0) || (iY >= m_iRows))
	{
		return false;
	}

	return true;
}


inline int CvAStar::udFunc(CvAStarFunc func, CvAStarNode* param1, CvAStarNode* param2, int data, const void* cb)
{
	return (func) ? func(param1, param2, data, cb, this) : 1;
}

// C-style non-member functions (used by path finder)
int PathAdd(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int PathValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
#ifdef AUI_CONSTIFY
int PathDestValid(int iToX, int iToY, const void* pointer, const CvAStar* finder);

int PathDest(int iToX, int iToyY, const void* pointer, const CvAStar* finder);
#else
int PathDestValid(int iToX, int iToY, const void* pointer, CvAStar* finder);

int PathDest(int iToX, int iToyY, const void* pointer, CvAStar* finder);
#endif
int PathHeuristic(int iFromX, int iFromY, int iToX, int iToY);
int PathCost(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int PathNodeAdd(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
#ifdef AUI_CONSTIFY
int IgnoreUnitsDestValid(int iToX, int iToY, const void* pointer, const CvAStar* finder);
#else
int IgnoreUnitsDestValid(int iToX, int iToY, const void* pointer, CvAStar* finder);
#endif
int IgnoreUnitsCost(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int IgnoreUnitsValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int IgnoreUnitsPathAdd(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
#ifdef AUI_CONSTIFY
int StepDestValid(int iToX, int iToY, const void* pointer, const CvAStar* finder);
#else
int StepDestValid(int iToX, int iToY, const void* pointer, CvAStar* finder);
#endif
int StepHeuristic(int iFromX, int iFromY, int iToX, int iToY);
int StepValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int StepValidAnyArea(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int StepCost(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int StepAdd(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int RouteValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int RouteGetNumExtraChildren(CvAStarNode* node,  CvAStar* finder);
int RouteGetExtraChild(CvAStarNode* node, int iIndex, int& iX, int& iY, CvAStar* finder);
int WaterRouteValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int AreaValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int JoinArea(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int LandmassValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int JoinLandmass(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
#ifdef AUI_CONSTIFY
int InfluenceDestValid(int iToX, int iToY, const void* pointer, const CvAStar* finder);
#else
int InfluenceDestValid(int iToX, int iToY, const void* pointer, CvAStar* finder);
#endif
int InfluenceHeuristic(int iFromX, int iFromY, int iToX, int iToY);
int InfluenceValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int InfluenceCost(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int InfluenceAdd(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int BuildRouteCost(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int BuildRouteValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int UIPathAdd(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int UIPathValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int AttackPathAdd(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
#ifdef AUI_CONSTIFY
int AttackPathDest(int iToX, int iToY, const void* pointer, const CvAStar* finder);
int AttackFortifiedPathDest(int iToX, int iToY, const void* pointer, const CvAStar* finder);
int AttackCityPathDest(int iToX, int iToY, const void* pointer, const CvAStar* finder);
#else
int AttackPathDest(int iToX, int iToY, const void* pointer, CvAStar* finder);
int AttackFortifiedPathDest(int iToX, int iToY, const void* pointer, CvAStar* finder);
int AttackCityPathDest(int iToX, int iToY, const void* pointer, CvAStar* finder);
#endif
int TacticalAnalysisMapPathValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
#ifdef AUI_CONSTIFY
int FindValidDestinationDest(int iToX, int iToY, const void* pointer, const CvAStar* finder);
#else
int FindValidDestinationDest(int iToX, int iToY, const void* pointer, CvAStar* finder);
#endif
int FindValidDestinationPathValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
#if defined(AUI_ASTAR_TURN_LIMITER)
int TurnsToReachTarget(UnitHandle pUnit, const CvPlot* pTarget, bool bReusePaths = false, bool bIgnoreUnits = false, bool bIgnoreStacking = false, int iTargetTurns = MAX_INT);
int TurnsToReachTargetFromPlot(UnitHandle pUnit, const CvPlot* pTarget, const CvPlot* pFromPlot, bool bReusePaths = false, bool bIgnoreUnits = false, bool bIgnoreStacking = false, int iTargetTurns = MAX_INT);
#else
int TurnsToReachTarget(UnitHandle pUnit, CvPlot* pTarget, bool bReusePaths=false, bool bIgnoreUnits=false, bool bIgnoreStacking=false);
#endif
bool CanReachInXTurns(UnitHandle pUnit, CvPlot* pTarget, int iTurns, bool bIgnoreUnits=false, int* piTurns = NULL);
#if defined(LEKMOD_HEADLESS_ASTAR_STOP_HOOK)
// OPT-0090: marks a stretch of code that only asks questions: no game state changes from the constructor to the destructor
// (the loops of FindClosestOperationUnit, FindClosestUnit, ...). Inside it TurnsToReachTarget may stop a failing search of
// GC.GetTacticalAnalysisMapFinder() early; when the outermost scope ends, the rest of the last such search is run, so the
// finder leaves the scope in the state the golden searches leave it (CvAStar.cpp, "OPT-0090"). Nests.
class CvHeadlessPathQueryScope
{
public:
	CvHeadlessPathQueryScope();
	~CvHeadlessPathQueryScope();
private:
	CvHeadlessPathQueryScope(const CvHeadlessPathQueryScope&);
	CvHeadlessPathQueryScope& operator=(const CvHeadlessPathQueryScope&);
};
#define HEADLESS_PATH_QUERY_SCOPE() CvHeadlessPathQueryScope kHeadlessPathQueryScope
// OPT-0090: TurnsToReachTarget for callers that only test the answer with <= iMaxTurns: the same answer when it is at most
// iMaxTurns, otherwise some value above iMaxTurns (inside a scope the search may stop as soon as that is certain)
int HeadlessTurnsToReachTargetAtMost(UnitHandle pUnit, CvPlot* pTarget, bool bReusePaths, bool bIgnoreUnits, bool bIgnoreStacking, int iMaxTurns);
#else
#define HEADLESS_PATH_QUERY_SCOPE()
#endif
#if defined(LEKMOD_HEADLESS_OPT_PAR_CLOSEST_OP_UNIT)
// OPT-0080: TurnsToReachTarget(pUnit, pTarget) (bReusePaths, bIgnoreUnits, bIgnoreStacking false, no turn limit) for a batch of
// units, the searches spread over the worker threads (per-thread clones of the tactical analysis map finder); afterwards the
// global finder holds the state of the batch's last search, as after the sequential loop. Simulation thread only.
struct CvHeadlessParTurnsItem
{
	CvUnit* pUnit;
	CvPlot* pTarget;
	int iTurns;		// out
};
void HeadlessParTurnsToReachTarget(CvHeadlessParTurnsItem* paItems, int iNumItems);
void HeadlessParShutdown();	// CvGlobals::uninit: stop the workers, free the finder clones
#endif
int TradeRouteHeuristic(int iFromX, int iFromY, int iToX, int iToY);
int TradeRouteLandPathCost(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int TradeRouteLandValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int TradeRouteWaterPathCost(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
int TradeRouteWaterValid(CvAStarNode* parent, CvAStarNode* node, int data, const void* pointer, CvAStar* finder);
void UnitPathInitialize(const void* pointer, CvAStar* finder);
void UnitPathUninitialize(const void* pointer, CvAStar* finder);
void TradePathInitialize(const void* pointer, CvAStar* finder);
void TradePathUninitialize(const void* pointer, CvAStar* finder);

#ifdef AUI_ASTAR_TWEAKED_OPTIMIZED_BUT_CAN_STILL_USE_ROADS
void IncreaseMoveRangeForRoads(const CvUnit* pUnit, int& iRange);
int GetIncreasedMoveRangeForRoads(const CvUnit* pUnit, int iRange);
#endif

// Derived classes (for more convenient access to pathfinding)
class CvTwoLayerPathFinder: public CvAStar
{
public:
	CvTwoLayerPathFinder();
	~CvTwoLayerPathFinder();
	void Initialize(int iColumns, int iRows, bool bWrapX, bool bWrapY, CvAPointFunc IsPathDestFunc, CvAPointFunc DestValidFunc, CvAHeuristic HeuristicFunc, CvAStarFunc CostFunc, CvAStarFunc ValidFunc, CvAStarFunc NotifyChildFunc, CvAStarFunc NotifyListFunc, CvABegin InitializeFunc, CvAEnd UninitializeFunc, const void* pData);
	void DeInit();
#ifdef AUI_ASTAR_MINOR_OPTIMIZATION
	inline CvAStarNode* GetPartialMoveNode(int iCol, int iRow) const;
#elif defined(AUI_CONSTIFY)
	CvAStarNode* GetPartialMoveNode(int iCol, int iRow) const;
#else
	CvAStarNode* GetPartialMoveNode(int iCol, int iRow);
#endif
	CvPlot* GetPathEndTurnPlot() const;

#ifdef AUI_ASTAR_TURN_LIMITER
	bool GenerateUnitPath(const CvUnit* pkUnit, int iXstart, int iYstart, int iXdest, int iYdest, int iInfo = 0, bool bReuse = false, int iTargetTurns = MAX_INT);
#else
	bool GenerateUnitPath(const CvUnit* pkUnit, int iXstart, int iYstart, int iXdest, int iYdest, int iInfo = 0, bool bReuse = false);
#endif

private:
	CvAStarNode** m_ppaaPartialMoveNodes;
};

class CvStepPathFinder: public CvAStar
{
public:
	int GetStepDistanceBetweenPoints(PlayerTypes ePlayer, PlayerTypes eEnemy, CvPlot* pStartPlot, CvPlot* pEndPlot);
	bool DoesPathExist(PlayerTypes ePlayer, PlayerTypes eEnemy, CvPlot* pStartPlot, CvPlot* pEndPlot);
	CvPlot* GetLastOwnedPlot(PlayerTypes ePlayer, PlayerTypes eEnemy, CvPlot* pStartPlot, CvPlot* pEndPlot) const;
	CvPlot* GetXPlotsFromEnd(PlayerTypes ePlayer, PlayerTypes eEnemy, CvPlot* pStartPlot, CvPlot* pEndPlot, int iPlotsFromEnd, bool bLeaveEnemyTerritory) const;
};

class CvIgnoreUnitsPathFinder: public CvAStar
{
public:
#if defined(AUI_ASTAR_TURN_LIMITER) && defined(AUI_ASTAR_MINOR_OPTIMIZATION)
	bool DoesPathExist(const CvUnit* pUnit, const CvPlot* pStartPlot, const CvPlot* pEndPlot, const int iMaxTurns = MAX_INT);
#elif defined(AUI_ASTAR_TURN_LIMITER)
	bool DoesPathExist(CvUnit& unit, CvPlot* pStartPlot, CvPlot* pEndPlot, const int iMaxTurns = MAX_INT);
#elif defined(AUI_ASTAR_MINOR_OPTIMIZATION)
	bool DoesPathExist(const CvUnit* pUnit, const CvPlot* pStartPlot, const CvPlot* pEndPlot);
#else
	bool DoesPathExist(CvUnit& unit, CvPlot* pStartPlot, CvPlot* pEndPlot);
#endif
	CvPlot* GetLastOwnedPlot(CvPlot* pStartPlot, CvPlot* pEndPlot, PlayerTypes iOwner) const;
#ifdef AUI_CONSTIFY
	int GetPathLength() const;
#else
	int GetPathLength();
#endif
	CvPlot* GetPathFirstPlot() const;
	CvPlot* GetPathEndTurnPlot() const;
	CvPlot* GetLastPlot();
	CvPlot* GetPreviousPlot();

private:
	CvAStarNode* m_pCurNode;
};

#if defined(LEKMOD_HEADLESS_FIX_0020)
// FIX-0020 / DS-0035: the IgnoreUnits finder for queries that run on one machine only, for its local UI (CvUnit::CanSwapWithUnitHere:
// the swap check of every human move/attack order and the exe's swap cursor). With the MP cache flag left on by
// CvPlayerAI::AI_unitUpdate's busy-unit early return, GC.getIgnoreUnitsPathFinder() reuses its last tree across turns in synced
// code (danger plots); a local search there replaced that tree on one machine only. Never use this finder in synced code.
CvIgnoreUnitsPathFinder& HeadlessUiIgnoreUnitsPathFinder();
#endif

#endif	//CVASTAR_H
