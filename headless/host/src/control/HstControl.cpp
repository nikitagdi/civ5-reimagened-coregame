//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//!	 \file		HstControl.cpp
//!  \brief     JSON-lines control channel (see HstControl.h). Every host-side step (parsing, formatting, paths)
//!				runs with the deterministic heap paused, so a game driven through the channel produces the same
//!				digests as the same game run from the command line.
//+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
#include "HstCommon.h"
#include "HstApi.h"
#include "control/HstControl.h"
#include "control/HstSession.h"
#include "platform/HstPlatform.h"
#include "platform/HstDetHeap.h"
#include "script/HstScriptSystem.h"
#include "engine/HstEngineUtility.h"
#include "game/HstJson.h"
#include "game/HstScenario.h"

//! Builder for one JSON object on one line.
class HstJsonLine
{
public:
	HstJsonLine() : m_bFirst(true) { m_str = "{"; }
	HstJsonLine& Str(const char* szKey, const std::string& strValue) { Key(szKey); m_str += "\"" + HstJsonEscape(strValue) + "\""; return *this; }
	HstJsonLine& Int(const char* szKey, int iValue) { char sz[32]; _snprintf_s(sz, sizeof(sz), _TRUNCATE, "%d", iValue); Key(szKey); m_str += sz; return *this; }
	HstJsonLine& UInt(const char* szKey, unsigned int uiValue) { char sz[32]; _snprintf_s(sz, sizeof(sz), _TRUNCATE, "%u", uiValue); Key(szKey); m_str += sz; return *this; }
	HstJsonLine& Num(const char* szKey, double dValue) { char sz[64]; _snprintf_s(sz, sizeof(sz), _TRUNCATE, "%.2f", dValue); Key(szKey); m_str += sz; return *this; }
	HstJsonLine& Bool(const char* szKey, bool bValue) { Key(szKey); m_str += bValue ? "true" : "false"; return *this; }
	HstJsonLine& Hex(const char* szKey, unsigned __int64 ui64) { char sz[32]; _snprintf_s(sz, sizeof(sz), _TRUNCATE, "\"%016I64x\"", ui64); Key(szKey); m_str += sz; return *this; }
	HstJsonLine& Raw(const char* szKey, const std::string& strJson) { Key(szKey); m_str += strJson; return *this; }
	std::string Done() const { return m_str + "}"; }

protected:
	void Key(const char* szKey)
	{
		if(!m_bFirst)
			m_str += ",";
		m_bFirst = false;
		m_str += "\"";
		m_str += szKey;
		m_str += "\":";
	}
	std::string m_str;
	bool m_bFirst;
};

class HstControlChannel : public HstTurnListener
{
public:
	HstControlChannel(HstSession& kSession, FILE* pkOut) : m_kSession(kSession), m_pkOut(pkOut), m_dLastOutput(HstNowMs()), m_iAlive(0) {}

	void Send(const std::string& strLine)
	{
		HstDetHeapPauseScope kPause;
		fputs(strLine.c_str(), m_pkOut);
		fputc('\n', m_pkOut);
		fflush(m_pkOut);
		m_dLastOutput = HstNowMs();
	}

	void Error(const std::string& strCmd, const std::string& strError)
	{
		HstDetHeapPauseScope kPause;
		Send(HstJsonLine().Bool("ok", false).Str("error", strError).Str("where", strCmd).Done());
	}

	virtual void OnTurn(const HstTurnRecord& kRecord)
	{
		HstDetHeapPauseScope kPause;
		HstJsonLine k;
		k.Str("event", "turn").Int("turn", kRecord.kTurn.iToTurn).UInt("slices", kRecord.kTurn.uiSlices).Num("ms", kRecord.kTurn.dMs)
			.Int("net", kRecord.kTurn.iNetMessages);
		if(kRecord.bDigest)
			k.Hex("digest", kRecord.ui64Combined).Num("digest_ms", kRecord.dDigestMs);
		if(!kRecord.strCheckpoint.empty())
			k.Str("checkpoint", kRecord.strCheckpoint);
		Send(k.Done());
	}

	virtual void OnSlice()
	{
		if(HstNowMs() - m_dLastOutput < 4000.0)
			return;
		HstDetHeapPauseScope kPause;
		Send(HstJsonLine().Str("event", "alive").Int("n", ++m_iAlive).Done());
	}

	//! Returns false on "quit".
	bool Handle(const std::string& strLine, int& iExitCode);

protected:
	//! Peer mode commands (peer, step, deliver, outbox, inject, end_turn, peer_state); false = not a peer command.
	bool HandlePeer(const std::string& strCmd, const HstJson& kReq);
	static std::string MessageJson(const HstNetMessage& kMsg);
	static bool ParseMessage(const HstJson& kItem, HstNetMessage& kMsg, std::string& strError);
	std::string OutboxJson();
	std::string GameInfoJson(HstJsonLine& k);
	std::vector<HstNetMessage> m_akPendingInbox;	// "deliver" before the next "step"
	std::wstring DefaultPath(const wchar_t* wszSub, const wchar_t* wszPrefix, const wchar_t* wszExt);

	HstSession& m_kSession;
	FILE* m_pkOut;
	double m_dLastOutput;
	int m_iAlive;
};

std::wstring HstControlChannel::DefaultPath(const wchar_t* wszSub, const wchar_t* wszPrefix, const wchar_t* wszExt)
{
	HstDetHeapPauseScope kPause;
	std::wstring wstrDir = wszSub && *wszSub ? HstJoinPath(HstGetRunDir(), wszSub) : HstGetRunDir();
	HstCreateDirs(wstrDir);
	wchar_t wszName[64];
	swprintf_s(wszName, L"%s-g%d-t%04d%s", wszPrefix, m_kSession.GetGameId(), m_kSession.GetGameTurn(), wszExt);
	return HstJoinPath(wstrDir, wszName);
}

std::string HstControlChannel::GameInfoJson(HstJsonLine& k)
{
	// DLL queries first (unpaused), formatting after.
	const int iTurn = m_kSession.GetGameTurn();
	const int iPlots = m_kSession.GetNumPlots();
	const int iPlayers = m_kSession.GetNumAlivePlayers();
	const HstHash64 ui64Start = m_kSession.ComputeCombinedDigest();
	HstDetHeapPauseScope kPause;
	int iLoaded = 0;
	const std::vector<HstScriptSystem::ContextInfo>& akContexts = m_kSession.GetScriptSystem()->GetContexts();
	for(size_t i = 0; i < akContexts.size(); ++i)
		iLoaded += akContexts[i].bLoaded ? 1 : 0;
	k.Bool("ok", true).Int("game_id", m_kSession.GetGameId()).Int("turn", iTurn).Int("plots", iPlots).Int("players_alive", iPlayers)
		.Hex("map_digest", m_kSession.GetMapDigest()).Hex("start_digest", ui64Start).Int("lua_contexts", (int)akContexts.size())
		.Int("lua_contexts_loaded", iLoaded).Int("lua_errors", m_kSession.GetScriptSystem()->GetErrorCount())
		.Str("digest_file", HstToUtf8(m_kSession.GetDigestPath()));
	return k.Done();
}

// ---- lockstep peer mode ------------------------------------------------------------------------------------------------
// Wire format of one message (JSON, one object): {"m": engine method, "p": sender player, "k": sender peer, "t": turn,
// "q": per-peer seq, "a": [int args, trailing zeros dropped], "s": text (only when non-empty)}.

std::string HstControlChannel::MessageJson(const HstNetMessage& kMsg)
{
	std::string strArgs = "[";
	int iLast = -1;
	for(int i = 0; i < (int)HST_ARRAY_SIZE(kMsg.aiArg); ++i)
		if(kMsg.aiArg[i] != 0)
			iLast = i;
	for(int i = 0; i <= iLast; ++i)
	{
		char sz[16];
		_snprintf_s(sz, sizeof(sz), _TRUNCATE, "%s%d", i ? "," : "", kMsg.aiArg[i]);
		strArgs += sz;
	}
	strArgs += "]";
	HstJsonLine k;
	k.Str("m", HstEngineUtility::MethodName(kMsg.eId)).Int("p", (int)kMsg.ePlayer).Int("k", kMsg.iPeer).Int("t", kMsg.iTurn).UInt("q", kMsg.uiSeq).Raw("a", strArgs);
	if(!kMsg.strText.empty())
		k.Str("s", kMsg.strText);
	return k.Done();
}

bool HstControlChannel::ParseMessage(const HstJson& kItem, HstNetMessage& kMsg, std::string& strError)
{
	const std::string strMethod = kItem.GetString("m", "");
	kMsg.eId = HstEngineUtility::MethodId(strMethod);
	if(kMsg.eId == (HstApiId)NUM_HST_API)
	{
		strError = "unknown message method '" + strMethod + "'";
		return false;
	}
	kMsg.ePlayer = (PlayerTypes)kItem.GetInt("p", -1);
	kMsg.iPeer = kItem.GetInt("k", 0);
	kMsg.iTurn = kItem.GetInt("t", 0);
	kMsg.uiSeq = kItem.GetUInt("q", 0);
	for(int i = 0; i < (int)HST_ARRAY_SIZE(kMsg.aiArg); ++i)
		kMsg.aiArg[i] = 0;
	if(const HstJson* pkArgs = kItem.Get("a"))
		for(size_t i = 0; i < pkArgs->Size() && i < HST_ARRAY_SIZE(kMsg.aiArg); ++i)
			kMsg.aiArg[i] = pkArgs->At(i).AsInt(0);
	kMsg.strText = kItem.GetString("s", "");
	return true;
}

std::string HstControlChannel::OutboxJson()
{
	std::vector<HstNetMessage> akOut;
	m_kSession.GetEngine()->TakeOutbox(akOut);
	std::string str = "[";
	for(size_t i = 0; i < akOut.size(); ++i)
	{
		if(i)
			str += ",";
		str += MessageJson(akOut[i]);
	}
	return str + "]";
}

bool HstControlChannel::HandlePeer(const std::string& strCmd, const HstJson& kReq)
{
	HstEngineUtility* pkEngine = m_kSession.GetEngine();
	if(strCmd == "peer")
	{
		// {"cmd":"peer","id":k,"peers":N,"host":0,"ai_sends":"all"|"owner","turn_barrier":false,"players":[p0,p1,...]}
		HstDetHeapPauseScope kPause;
		HstPeerConfig kCfg;
		kCfg.bEnabled = kReq.GetBool("enabled", true);
		kCfg.iPeerId = kReq.GetInt("id", 0);
		kCfg.iNumPeers = kReq.GetInt("peers", 1);
		kCfg.iHostPeer = kReq.GetInt("host", 0);
		const std::string strAi = kReq.GetString("ai_sends", "all");
		if(strAi != "all" && strAi != "owner")
		{
			Error(strCmd, "ai_sends must be all or owner");
			return true;
		}
		kCfg.bAiSendsOwner = strAi == "owner";
		kCfg.bTurnBarrier = kReq.GetBool("turn_barrier", false);
		const std::string strTurnModel = kReq.GetString("turn_model", "legacy");
		if(strTurnModel != "legacy" && strTurnModel != "exe")
		{
			Error(strCmd, "turn_model must be legacy or exe");
			return true;
		}
		kCfg.bExeTurns = strTurnModel == "exe";
		kCfg.bTurnSlices = kReq.GetBool("turn_slices", false);
		if(const HstJson* pkPlayers = kReq.Get("players"))
			for(size_t i = 0; i < pkPlayers->Size(); ++i)
				kCfg.aiPeerPlayers.push_back(pkPlayers->At(i).AsInt(-1));
		std::vector<std::string> astrMask;
		if(const HstJson* pkMask = kReq.Get("mask"))
			for(size_t i = 0; i < pkMask->Size(); ++i)
				astrMask.push_back(pkMask->At(i).AsString());
		HstDigest::SetMask(astrMask);
		if(kCfg.iNumPeers < 1 || kCfg.iNumPeers > MAX_PLAYERS || kCfg.iPeerId < 0 || kCfg.iPeerId >= kCfg.iNumPeers)
		{
			Error(strCmd, "need 0 <= id < peers <= 64");
			return true;
		}
		if(m_kSession.InGame())
			HstLog("peer: WARNING configuration changed while a game runs");
		pkEngine->SetPeerConfig(kCfg);
		HstLog("peer: id %d of %d, host %d, ai_sends %s, turn_barrier %d, turn_model %s, turn_slices %d", kCfg.iPeerId, kCfg.iNumPeers, kCfg.iHostPeer,
			strAi.c_str(), (int)kCfg.bTurnBarrier, strTurnModel.c_str(), (int)kCfg.bTurnSlices);
		Send(HstJsonLine().Bool("ok", true).Int("id", kCfg.iPeerId).Int("peers", kCfg.iNumPeers).Int("host", kCfg.iHostPeer).Str("ai_sends", strAi)
			.Bool("turn_barrier", kCfg.bTurnBarrier).Str("turn_model", strTurnModel).Bool("turn_slices", kCfg.bTurnSlices)
			.Bool("is_host", pkEngine->IsHost()).Done());
		return true;
	}
	if(strCmd != "step" && strCmd != "deliver" && strCmd != "outbox" && strCmd != "inject" && strCmd != "end_turn" && strCmd != "peer_state"
		&& strCmd != "bot")
		return false;
	if(!pkEngine->IsPeerMode())
	{
		Error(strCmd, "not in peer mode (send {\"cmd\":\"peer\",...} before new_game)");
		return true;
	}
	if(strCmd == "deliver")
	{
		HstDetHeapPauseScope kPause;
		std::string strError;
		const HstJson* pkMsgs = kReq.Get("msgs");
		const size_t uiBefore = m_akPendingInbox.size();
		for(size_t i = 0; pkMsgs && i < pkMsgs->Size(); ++i)
		{
			HstNetMessage kMsg;
			if(!ParseMessage(pkMsgs->At(i), kMsg, strError))
			{
				m_akPendingInbox.resize(uiBefore);
				Error(strCmd, strError);
				return true;
			}
			m_akPendingInbox.push_back(kMsg);
		}
		Send(HstJsonLine().Bool("ok", true).Int("pending", (int)m_akPendingInbox.size()).Done());
		return true;
	}
	if(strCmd == "outbox")
	{
		HstDetHeapPauseScope kPause;
		Send(HstJsonLine().Bool("ok", true).Raw("outbox", OutboxJson()).Done());
		return true;
	}
	if(strCmd == "peer_state")
	{
		HstDetHeapPauseScope kPause;
		const PlayerTypes eActive = pkEngine->GetActivePlayerForHost();
		std::string strDone = "[";
		for(int i = 0; i < MAX_PLAYERS; ++i)
			if(pkEngine->HasTurnCompleteReceived((PlayerTypes)i))
			{
				char sz[16];
				_snprintf_s(sz, sizeof(sz), _TRUNCATE, "%s%d", strDone.size() > 1 ? "," : "", i);
				strDone += sz;
			}
		strDone += "]";
		Send(HstJsonLine().Bool("ok", true).Int("active_player", (int)eActive).Int("turn", m_kSession.GetGameTurn())
			.Int("ai_processed_peers", pkEngine->GetNumAICivsProcessedPeers()).Bool("all_ai_processed", pkEngine->AICivsProcessedReceived())
			.Raw("turn_complete_received", strDone).Int("want_advance", pkEngine->GetAdvanceWanted()).Int("sent", pkEngine->GetPeerSent())
			.Int("dropped", pkEngine->GetPeerDropped()).Int("received", pkEngine->GetPeerReceived()).Bool("is_host", pkEngine->IsHost())
			.Bool("turn_slicing", pkEngine->IsTurnSlicing()).Int("slice", pkEngine->GetTurnSliceNumber()).Int("send_queue", pkEngine->GetSendQueueSize())
			.Int("flushes", pkEngine->GetTurnSliceFlushes()).Int("flushed", pkEngine->GetTurnSliceFlushedMessages()).Done());
		return true;
	}
	if(!m_kSession.InGame())
	{
		Error(strCmd, "no game (new_game first)");
		return true;
	}
	if(strCmd == "bot")
	{
		// {"cmd":"bot","file":"<bot.lua>","init":"<Lua chunk run first (seed, config)>"}: load the local UI bot of this peer.
		std::wstring wstrFile;
		std::string strInit, strError;
		{
			HstDetHeapPauseScope kPause;
			wstrFile = HstFromUtf8(kReq.GetString("file", ""));
			strInit = kReq.GetString("init", "");
		}
		if(!m_kSession.BotLoad(wstrFile, strInit, strError))
		{
			Error(strCmd, strError);
			return true;
		}
		Send(HstJsonLine().Bool("ok", true).Str("bot", "loaded").Done());
		return true;
	}
	if(strCmd == "inject" || strCmd == "end_turn")
	{
		// Scripted bot: a command as if the local player's UI issued it (goes to the outbox; delivered at the next frame).
		std::string strMethod, strText, strError;
		std::vector<int> aiArgs;
		PlayerTypes eFrom = NO_PLAYER;
		{
			HstDetHeapPauseScope kPause;
			strMethod = strCmd == "end_turn" ? std::string("sendTurnComplete") : kReq.GetString("method", "");
			if(const HstJson* pkArgs = kReq.Get("args"))
				for(size_t i = 0; i < pkArgs->Size(); ++i)
					aiArgs.push_back(pkArgs->At(i).AsInt(0));
			strText = kReq.GetString("text", "");
			eFrom = (PlayerTypes)kReq.GetInt("from", -1);
		}
		HstDetHeapPauseScope kPause;	// host-side queueing only; no DLL code runs here
		if(!pkEngine->InjectByName(strMethod, aiArgs, strText, eFrom, strError))
		{
			Error(strCmd, strError);
			return true;
		}
		Send(HstJsonLine().Bool("ok", true).Str("method", strMethod).Done());
		return true;
	}
	// step: {"cmd":"step","inbox":[msgs],"grant":T,"digest_every":1,"dump_state_at":-1}
	std::vector<HstNetMessage> akInbox;
	HstStepRequest kStep;
	{
		HstDetHeapPauseScope kPause;
		std::string strError;
		akInbox.swap(m_akPendingInbox);
		if(const HstJson* pkMsgs = kReq.Get("inbox"))
			for(size_t i = 0; i < pkMsgs->Size(); ++i)
			{
				HstNetMessage kMsg;
				if(!ParseMessage(pkMsgs->At(i), kMsg, strError))
				{
					Error(strCmd, strError);
					return true;
				}
				akInbox.push_back(kMsg);
			}
		kStep.iGrantTurn = kReq.GetInt("grant", -1);
		kStep.iDigestEvery = kReq.GetInt("digest_every", 1);
		kStep.iDumpStateAt = kReq.GetInt("dump_state_at", -1);
	}
	std::string strBotMode;
	{
		HstDetHeapPauseScope kPause;
		strBotMode = kReq.GetString("bot", "");
	}
	HstStepResult kResult;
	std::string strError;
	if(!m_kSession.Step(akInbox, kStep, kResult, strError))
	{
		Error(strCmd, strError);
		return true;
	}
	// The bot (local UI) acts between game-core updates, after the frame's Update and digest; its sends join this outbox.
	std::string strBotLog;
	if(!strBotMode.empty() && m_kSession.HasBot())
		strBotLog = m_kSession.BotTick(kResult.iFrame, strBotMode.c_str());
	HstDetHeapPauseScope kPause;
	akInbox.clear();
	HstJsonLine k;
	k.Bool("ok", true).Int("frame", kResult.iFrame).Int("turn", kResult.iTurn).Int("from_turn", kResult.iFromTurn).Bool("turn_changed", kResult.bTurnChanged)
		.Int("want_advance", pkEngine->GetAdvanceWanted()).Int("ai_processed_peers", pkEngine->GetNumAICivsProcessedPeers())
		.Bool("all_ai", pkEngine->AICivsProcessedReceived()).Bool("tc_sent", pkEngine->TurnCompleteSentLocal()).Num("ms", kResult.dMs)
		.Int("lua_errors", m_kSession.GetScriptSystem()->GetErrorCount());
	if(pkEngine->IsExeTurns())
		k.Int("barrier_turn", pkEngine->GetBarrierTurn()).Int("all_ai_turn", pkEngine->GetAllAIProcessedTurn()).Bool("tac_sent", pkEngine->SentTurnAllComplete());
	if(pkEngine->IsTurnSlicing())
		k.Int("slice", pkEngine->GetTurnSliceNumber()).Int("sq", pkEngine->GetSendQueueSize());
	if(!strBotLog.empty())
		k.Str("bot", strBotLog);
	if(kResult.bTurnChanged)
	{
		k.UInt("frames", kResult.kRecord.kTurn.uiSlices);
		if(kResult.kRecord.bDigest)
			k.Hex("digest", kResult.kRecord.ui64Combined);
		if(kResult.bMasked)
			k.Hex("mdigest", kResult.ui64Masked).UInt("masked_bytes", kResult.uiMaskedBytes);
	}
	k.Raw("outbox", OutboxJson());
	Send(k.Done());
	return true;
}

bool HstControlChannel::Handle(const std::string& strLine, int& iExitCode)
{
	HstJson kReq;
	std::string strError, strCmd;
	{
		HstDetHeapPauseScope kPause;
		if(!HstJson::Parse(strLine, kReq, strError) || kReq.GetType() != HstJson::JSON_OBJECT)
		{
			Error("parse", strError.empty() ? "request must be a JSON object" : strError);
			return true;
		}
		strCmd = kReq.GetString("cmd", "");
		if(strCmd != "step" || !m_kSession.GetEngine()->IsPeerMode())
			HstLog("control: %s", strLine.c_str());
	}

	if(HandlePeer(strCmd, kReq))
		return true;
	if(strCmd == "run" && m_kSession.GetEngine()->IsPeerMode())
	{
		Error(strCmd, "peer mode: use step (the coordinator drives the frames)");
		return true;
	}
	if(strCmd == "hello")
	{
		HstDetHeapPauseScope kPause;
		const HstSessionOptions& kOpt = m_kSession.GetOptions();
		Send(HstJsonLine().Bool("ok", true).Str("cmd", "hello").Str("host", HST_HOST_VERSION).Str("dll", HstToUtf8(kOpt.wstrDll))
			.Str("dll_version", m_kSession.GetDllVersion()).Hex("dll_xxh64", m_kSession.GetDllHash()).Str("data", HstToUtf8(kOpt.wstrData))
			.Hex("data_xxh64", m_kSession.GetDataHash()).Str("lua_overlay", HstToUtf8(kOpt.wstrLuaOverlay)).Str("mode", m_kSession.GetEngine()->IsPeerMode() ? "peer" : "single")
			.Str("run_dir", HstToUtf8(HstGetRunDir())).UInt("pid", (unsigned int)GetCurrentProcessId()).Bool("in_game", m_kSession.InGame())
			.Bool("allow_warm", kOpt.bAllowWarm).UInt("games_hosted", (unsigned int)m_kSession.GetGameId()).Done());
		return true;
	}
	if(strCmd == "new_game")
	{
		HstScenario kScenario;
		bool bOk = false;
		{
			HstDetHeapPauseScope kPause;
			const HstJson* pkScenario = kReq.Get("scenario");
			if(!pkScenario)
				strError = "new_game needs \"scenario\" (path or object)";
			else if(pkScenario->GetType() == HstJson::JSON_STRING)
				bOk = kScenario.Load(m_kSession.ResolveUserPath(pkScenario->AsString()), strError);
			else
				bOk = kScenario.LoadJson(*pkScenario, strError);
			if(bOk && kReq.Get("active_player"))
			{
				const HstJson* pkActive = kReq.Get("active_player");
				if(pkActive->GetType() == HstJson::JSON_STRING)
					kScenario.SetActivePlayer(pkActive->AsString());
				else
				{
					char sz[16];
					_snprintf_s(sz, sizeof(sz), _TRUNCATE, "%d", pkActive->AsInt());
					kScenario.SetActivePlayer(sz);
				}
			}
			if(bOk && kReq.Get("game_type"))
				kScenario.SetGameType(kReq.GetString("game_type", "single"));
			if(bOk && kReq.Get("game_options_preset"))
				bOk = kScenario.SetGameOptionsPreset(kReq.GetString("game_options_preset", ""), strError);
		}
		if(!bOk)
		{
			Error(strCmd, strError);
			return true;
		}
		if(!m_kSession.NewGame(kScenario, strError))
		{
			Error(strCmd, strError);
			return true;
		}
		HstJsonLine k;
		Send(GameInfoJson(k));
		return true;
	}
	if(strCmd == "load")
	{
		std::wstring wstrPath;
		{
			HstDetHeapPauseScope kPause;
			wstrPath = m_kSession.ResolveUserPath(kReq.GetString("path", ""));
		}
		if(!m_kSession.LoadGame(wstrPath, strError))
		{
			Error(strCmd, strError);
			return true;
		}
		HstJsonLine k;
		Send(GameInfoJson(k));
		return true;
	}
	if(strCmd == "import_save")
	{
		// {"cmd":"import_save","dir":"<output of python -m civsim.saveio extract>","keep_humans":false}
		std::wstring wstrDir;
		bool bKeepHumans = false;
		{
			HstDetHeapPauseScope kPause;
			wstrDir = m_kSession.ResolveUserPath(kReq.GetString("dir", ""));
			bKeepHumans = kReq.GetBool("keep_humans", false);
		}
		if(!m_kSession.ImportSave(wstrDir, bKeepHumans, strError))
		{
			Error(strCmd, strError);
			return true;
		}
		HstJsonLine k;
		Send(GameInfoJson(k));
		return true;
	}
	if(strCmd == "run")
	{
		if(!m_kSession.InGame())
		{
			Error(strCmd, "no game (new_game or load first)");
			return true;
		}
		HstRunRequest kRun;
		{
			HstDetHeapPauseScope kPause;
			kRun.iTurns = kReq.GetInt("turns", 1);
			kRun.iUntilTurn = kReq.GetInt("until_turn", -1);
			kRun.iDigestEvery = kReq.GetInt("digest_every", 1);
			kRun.iCheckpointEvery = kReq.GetInt("checkpoint_every", 0);
			kRun.iDumpStateAt = kReq.GetInt("dump_state_at", -1);
			if(const HstJson* pkStop = kReq.Get("stop_on"))
				for(size_t i = 0; i < pkStop->Size(); ++i)
					if(pkStop->At(i).AsString() == "lua_error")
						kRun.bStopOnLuaError = true;
		}
		const double dStart = HstNowMs();
		HstRunResult kResult = m_kSession.Run(kRun, this);
		m_kSession.WriteRunJson(kResult.strReason == "done" ? "ok" : kResult.strReason);
		HstDetHeapPauseScope kPause;
		Send(HstJsonLine().Bool("ok", true).Int("turn", kResult.iEndTurn).Int("start_turn", kResult.iStartTurn).Int("turns_done", kResult.iTurnsDone)
			.Str("reason", kResult.strReason).Str("digest_file", HstToUtf8(m_kSession.GetDigestPath())).Str("run_json", HstToUtf8(HstJoinPath(HstGetRunDir(), L"run.json")))
			.Raw("ms", HstJsonLine().Num("turns", kResult.dTotalMs).Num("total", HstNowMs() - dStart).Done())
			.Int("lua_errors", m_kSession.GetScriptSystem()->GetErrorCount()).Done());
		return true;
	}
	if(strCmd == "save")
	{
		std::wstring wstrPath;
		{
			HstDetHeapPauseScope kPause;
			std::string strPath = kReq.GetString("path", "");
			wstrPath = strPath.empty() ? DefaultPath(L"checkpoints", L"save", L".ckpt") : m_kSession.ResolveUserPath(strPath);
		}
		unsigned int uiBytes = 0;
		HstHash64 ui64Hash = 0;
		if(!m_kSession.SaveGame(wstrPath, uiBytes, ui64Hash, strError))
		{
			Error(strCmd, strError);
			return true;
		}
		HstDetHeapPauseScope kPause;
		Send(HstJsonLine().Bool("ok", true).Str("path", HstToUtf8(wstrPath)).UInt("bytes", uiBytes).Hex("xxh64", ui64Hash).Int("turn", m_kSession.GetGameTurn()).Done());
		return true;
	}
	if(strCmd == "digest")
	{
		std::string strLevel;
		std::wstring wstrPath;
		{
			HstDetHeapPauseScope kPause;
			strLevel = kReq.GetString("level", "subsystem");
			if(strLevel != "turn" && strLevel != "subsystem" && strLevel != "full")
				strLevel = "";
			std::string strPath = kReq.GetString("path", "");
			if(!strLevel.empty())
				wstrPath = !strPath.empty() ? m_kSession.ResolveUserPath(strPath) :
					DefaultPath(L"digests", HstFromUtf8(strLevel).c_str(), strLevel == "turn" ? L".txt" : (strLevel == "subsystem" ? L".bin" : L""));
		}
		if(strLevel.empty())
		{
			Error(strCmd, "level must be turn, subsystem or full");
			return true;
		}
		HstHash64 ui64Combined = 0;
		if(!m_kSession.WriteDigest(strLevel, wstrPath, ui64Combined, strError))
		{
			Error(strCmd, strError);
			return true;
		}
		HstDetHeapPauseScope kPause;
		Send(HstJsonLine().Bool("ok", true).Str("level", strLevel).Str("path", HstToUtf8(wstrPath)).Int("turn", m_kSession.GetGameTurn())
			.Hex("digest", ui64Combined).Done());
		return true;
	}
	if(strCmd == "end_game")
	{
		m_kSession.EndGame();
		HstDetHeapPauseScope kPause;
		Send(HstJsonLine().Bool("ok", true).Done());
		return true;
	}
	if(strCmd == "quit")
	{
		if(m_kSession.InGame())
			m_kSession.EndGame();
		HstDetHeapPauseScope kPause;
		Send(HstJsonLine().Bool("ok", true).Done());
		iExitCode = 0;
		return false;
	}
	Error(strCmd.empty() ? "parse" : strCmd, "unknown command (hello, new_game, load, run, save, digest, end_game, quit; peer mode: peer, step, deliver, "
		"outbox, inject, end_turn, peer_state)");
	return true;
}

int HstControlRun(HstSession& kSession, FILE* pkOut, FILE* pkIn)
{
	HstControlChannel kChannel(kSession, pkOut);
	HstLog("control: stdio channel ready");
	int iExitCode = 0;
	std::string strLine;
	char szBuf[4096];
	for(;;)
	{
		{
			HstDetHeapPauseScope kPause;
			strLine.clear();
			bool bGot = false;
			while(fgets(szBuf, sizeof(szBuf), pkIn))
			{
				bGot = true;
				strLine += szBuf;
				if(!strLine.empty() && strLine[strLine.size() - 1] == '\n')
					break;
			}
			if(!bGot)
			{
				HstLog("control: stdin closed");
				break;
			}
			while(!strLine.empty() && (strLine[strLine.size() - 1] == '\n' || strLine[strLine.size() - 1] == '\r'))
				strLine.erase(strLine.size() - 1);
			if(strLine.find_first_not_of(" \t") == std::string::npos)
				continue;
		}
		if(!kChannel.Handle(strLine, iExitCode))
			break;
	}
	return iExitCode;
}

bool HstControlSelfTest(std::string& strDetail)
{
	HstJson kReq;
	std::string strError;
	if(!HstJson::Parse("{\"cmd\":\"run\",\"turns\":5,\"stop_on\":[\"lua_error\"]}", kReq, strError))
	{
		strDetail = "parse failed: " + strError;
		return false;
	}
	const std::string strLine = HstJsonLine().Bool("ok", true).Str("s", "a\"b").Hex("h", 0xabcULL).Int("i", -3).Done();
	strDetail = strLine;
	return kReq.GetString("cmd", "") == "run" && kReq.GetInt("turns", 0) == 5 && kReq.Get("stop_on")->Size() == 1 &&
		strLine == "{\"ok\":true,\"s\":\"a\\\"b\",\"h\":\"0000000000000abc\",\"i\":-3}";
}
