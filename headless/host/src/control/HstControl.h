//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstControl.h
//!  \brief     --control stdio: JSON-lines control channel.
//!
//!		One command per stdin line, one response per stdout line; during "run" the host also emits
//!		{"event":"turn",...} after every turn and {"event":"alive",...} at least every 5 s. Commands:
//!		hello, new_game, load, run, save, digest, end_game, quit. Errors: {"ok":false,"error":"...","where":"<cmd>"}.
//!		Bulk data (digests, checkpoints) goes to files in the run dir; responses carry path, size and hash.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#pragma once
#ifndef HSTCONTROL_H
#define HSTCONTROL_H

#include "HstCommon.h"

class HstSession;

//! Reads commands from pkIn until "quit" or EOF. Returns the process exit code.
int HstControlRun(HstSession& kSession, FILE* pkOut, FILE* pkIn);
//! Host-only check of request parsing and response formatting (--selftest).
bool HstControlSelfTest(std::string& strDetail);

#endif // HSTCONTROL_H
