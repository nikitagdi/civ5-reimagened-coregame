//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstRngLog.h
//!  \brief     RNG call stream of the diag gamecore DLLs: per-turn stream hashes and a
//!				windowed full call log, fed by the DLL's HeadlessDiagInstall sink (HstDiagSink.h).
//!
//!		--rng-digest   <run>\rng-digest.bin   per phase (pregame, every turn): per RNG kind calls, reseeds, first/last seed
//!		                                      and an address-independent hash of the (seed, range, result) stream.
//!		--rng-log A-B  <run>\rng.log.bin      every call of the turns A..B (20 bytes each), plus <run>\rng.sites.tsv: call
//!		                                      site table (pszLog text, caller address resolved through the DLL's PDB).
//!		Formats: section "RNG call log". Only diag / optimized-diag DLLs export
//!		HeadlessDiagInstall; for the others the flags log a note and write nothing. The sink never allocates from the
//!		deterministic heap and never touches the FPU, so digest.bin is identical with and without it.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTRNGLOG_H
#define HSTRNGLOG_H

#include "HstCommon.h"

struct HstRngLogOptions
{
	bool bDigest;		// --rng-digest
	bool bLog;			// --rng-log
	int iLogFrom;		// first turn of the window, -1 = from the pregame phase (game creation / checkpoint load)
	int iLogTo;			// last turn of the window (inclusive)

	HstRngLogOptions() : bDigest(false), bLog(false), iLogFrom(0), iLogTo(0) {}
	bool Any() const { return bDigest || bLog; }
};

class HstRngLog
{
public:
	//! "A-B" (turns inclusive), "A", "pre" (pregame only) or "pre-B". false on a syntax error.
	static bool ParseWindow(const std::string& strWindow, int& iFrom, int& iTo);
	//! Installs the sink into the loaded DLL (call after LoadLibrary, before ctx->Init()). false: the DLL exports no
	//! HeadlessDiagInstall or speaks another version; nothing is recorded then.
	static bool Install(HMODULE hDll, const HstRngLogOptions& kOpt);
	static bool IsInstalled();
	//! Opens the output files of game iGameId in the run dir (closes the previous game's). Game 1: rng-digest.bin,
	//! rng.log.bin, rng.sites.tsv; game N: rng-digest-N.bin, rng-N.log.bin, rng-N.sites.tsv.
	static void BeginGame(int iGameId);
	//! A phase is the pregame (iTurn -1: game creation or checkpoint load) or one turn (iTurn = turn being played).
	static void BeginPhase(int iTurn);
	//! Start of an Update slice inside a turn phase (the log's slice number).
	static void NextSlice();
	static void EndPhase();
	//! Pending log bytes to disk; rewrites the call site table.
	static void Flush();
	static void EndGame();
};

//! Pregame phase for the duration of a scope (NewGame / LoadGame bodies).
struct HstRngPhaseScope
{
	explicit HstRngPhaseScope(int iTurn) { HstRngLog::BeginPhase(iTurn); }
	~HstRngPhaseScope() { HstRngLog::EndPhase(); }
};

#endif // HSTRNGLOG_H
