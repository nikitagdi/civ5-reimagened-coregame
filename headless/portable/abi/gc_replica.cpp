// B4 gc-replica: GCJ1 / GCN1 codec and the input journal of the process (gc_replica.h).
#include "CvGameCoreDLLPCH.h"

#include "gc_replica.h"

#include <string.h>

#include "../runtime/rt_bytes.h"

namespace lekmod_portable {

namespace {

const char kJournalMagic[4] = {'G', 'C', 'J', '1'};
const char kNetMagic[4] = {'G', 'C', 'N', '1'};
const uint32_t kFormatVersion = 1;
const uint32_t kMaxRecords = 1u << 20;
const uint32_t kNetArgs = 9;

bool g_journalOn = false;
std::vector<JournalRecord> g_journal;

const char* const kNetNames[kRtNet_Count] = {"None", "sendTurnComplete", "sendTurnUnready", "SendAICivsProcessed", "IdeologyChoice", "ArchaeologyChoice",
	"SendDiploVote", "SendEnhanceReligion", "SendFaithPurchase", "SendFoundPantheon", "SendFoundReligion", "SendLeagueEditName", "SendLeagueProposeEnact",
	"SendLeagueProposeRepeal", "SendLeagueVoteAbstain", "SendLeagueVoteEnact", "SendLeagueVoteRepeal", "SendLiberateMinor", "SendMinorNoUnitSpawning",
	"SendMoveSpy", "SendPledgeMinorProtection", "SendRenameCity", "SendRenameUnit", "SendSellBuilding", "SendStageCoup", "SendUpdateCityCitizens",
	"sendAdvancedStartAction", "sendAutoMission", "sendBarbarianRansom", "sendChangeIdeology", "sendChangeWar", "sendCityBuyPlot", "sendDoCommand", "sendDoTask",
	"sendFaithGreatPersonChoice", "sendFromUIDiploEvent", "sendGiftUnit", "sendGoodyChoice", "sendGreatPersonChoice", "sendIgnoreWarning", "sendLaunch",
	"sendMayaBonusChoice", "sendMinorBullyGold", "sendMinorBullyUnit", "sendMinorBuyout", "sendMinorCivEnterTerritory", "sendMinorCivIntrusion",
	"sendMinorCivQuestCompleted", "sendMinorCivQuestNoInterest", "sendMinorGiftGold", "sendMinorGiftTileImprovement", "sendMinorPledgeProtection",
	"sendMoveGreatWorks", "sendPlayerHurry", "sendPopOrder", "sendPurchase_1", "sendPurchase_2", "sendPushMission", "sendPushOrder", "sendResearch",
	"sendReturnCivilian", "sendSetCityAIFocus", "sendSetCityAvoidGrowth", "sendSetSwappableGreatWork", "sendSwapGreatWorks", "sendSwapOrder", "sendSwapUnits",
	"sendUpdatePolicies", "sendAutoMoves", "sendNetDealAccepted", "sendNetDemandAccepted", "sendNetDealProposed", "sendNetPlayerDealFinalized"};

void WriteText(RtByteWriter& w, const std::string& s)
{
	const size_t n = s.size() > 0xFFFEu ? 0xFFFEu : s.size();
	w.U16((uint32_t)n);
	w.Bytes(s.data(), n);
}

bool ReadText(RtByteReader& r, std::string& s)
{
	const uint32_t n = r.U16();
	const uint8_t* p = r.Take(n);
	if (!r.Ok()) return false;
	s.assign((const char*)p, n);
	return true;
}

void WriteNetBody(RtByteWriter& w, const RtNetMessage& m)
{
	w.I32(m.eId);
	for (uint32_t i = 0; i < kNetArgs; ++i) w.I32(m.aiArg[i]);
	WriteText(w, m.strText);
}

bool ReadNetBody(RtByteReader& r, RtNetMessage& m)
{
	m.eId = r.I32();
	for (uint32_t i = 0; i < kNetArgs; ++i) m.aiArg[i] = r.I32();
	return ReadText(r, m.strText) && r.Ok() && m.eId > kRtNet_None && m.eId < kRtNet_Count;
}

bool ReadHeader(RtByteReader& r, const char magic[4], uint32_t& count, std::string& why)
{
	const uint8_t* m = r.Take(4);
	if (!r.Ok() || memcmp(m, magic, 4) != 0)
	{
		why = std::string("not ") + std::string(magic, 4) + " data";
		return false;
	}
	const uint32_t version = r.U16();
	r.U16();
	count = r.U32();
	if (!r.Ok() || version != kFormatVersion || count > kMaxRecords)
	{
		why = "unknown format version or a bad record count";
		return false;
	}
	return true;
}

}  // namespace

void JournalEnable(bool on)
{
	g_journalOn = on;
	if (!on) g_journal.clear();
}
bool JournalOn() { return g_journalOn; }
void JournalAppend(const JournalRecord& r)
{
	if (g_journalOn) g_journal.push_back(r);
}
size_t JournalSize() { return g_journal.size(); }

void JournalTake(std::vector<uint8_t>& out)
{
	EncodeJournal(g_journal, out);
	g_journal.clear();
}

void EncodeJournal(const std::vector<JournalRecord>& recs, std::vector<uint8_t>& out)
{
	out.clear();
	RtByteWriter w(out);
	w.Bytes(kJournalMagic, 4);
	w.U16(kFormatVersion);
	w.U16(0);
	w.U32((uint32_t)recs.size());
	for (size_t i = 0; i < recs.size(); ++i)
	{
		const JournalRecord& r = recs[i];
		std::vector<uint8_t> payload;
		RtByteWriter p(payload);
		switch (r.type)
		{
		case GC_JOURNAL_COMMAND:
			p.U16((uint32_t)r.kind);
			p.U16((uint32_t)r.args.size());
			for (size_t a = 0; a < r.args.size(); ++a) p.I32(r.args[a]);
			p.U16(r.hasText ? (uint32_t)(r.text.size() > 0xFFFEu ? 0xFFFEu : r.text.size()) : 0xFFFFu);
			if (r.hasText) p.Bytes(r.text.data(), r.text.size() > 0xFFFEu ? 0xFFFEu : r.text.size());
			break;
		case GC_JOURNAL_NET: WriteNetBody(p, r.net); break;
		case GC_JOURNAL_PUMP:
			p.U32(r.slices);
			p.I32(r.status);
			p.I32(r.turn);
			break;
		case GC_JOURNAL_SLOT:
			p.I32(r.op);
			p.U16(r.hasText ? (uint32_t)r.text.size() : 0xFFFFu);
			if (r.hasText) p.Bytes(r.text.data(), r.text.size());
			break;
		case GC_JOURNAL_CHECK: p.U64(r.digest); break;
		default: break;
		}
		w.U8((uint32_t)r.type);
		w.U8((uint32_t)(r.player & 0xFF));
		w.U16((uint32_t)payload.size());
		w.Bytes(payload.empty() ? NULL : &payload[0], payload.size());
	}
}

bool DecodeJournal(const uint8_t* p, size_t n, std::vector<JournalRecord>& recs, std::string& why)
{
	recs.clear();
	RtByteReader r(p, n);
	uint32_t count = 0;
	if (!ReadHeader(r, kJournalMagic, count, why)) return false;
	for (uint32_t i = 0; i < count; ++i)
	{
		JournalRecord rec;
		rec.type = (int)r.U8();
		const uint32_t player = r.U8();
		rec.player = player == 0xFF ? -1 : (int)player;
		const uint32_t len = r.U16();
		const uint8_t* body = r.Take(len);
		if (!r.Ok())
		{
			why = "truncated record";
			return false;
		}
		RtByteReader b(body, len);
		switch (rec.type)
		{
		case GC_JOURNAL_COMMAND:
		{
			rec.kind = (int)b.U16();
			const uint32_t argc = b.U16();
			for (uint32_t a = 0; a < argc && b.Ok(); ++a) rec.args.push_back(b.I32());
			const uint32_t tl = b.U16();
			rec.hasText = tl != 0xFFFFu;
			if (rec.hasText)
			{
				const uint8_t* t = b.Take(tl);
				if (b.Ok()) rec.text.assign((const char*)t, tl);
			}
			break;
		}
		case GC_JOURNAL_NET:
			rec.net.ePlayer = (PlayerTypes)rec.player;
			if (!ReadNetBody(b, rec.net)) b.Fail();
			break;
		case GC_JOURNAL_PUMP:
			rec.slices = b.U32();
			rec.status = b.I32();
			rec.turn = b.I32();
			break;
		case GC_JOURNAL_SLOT:
		{
			rec.op = b.I32();
			const uint32_t tl = b.U16();
			rec.hasText = tl != 0xFFFFu;
			if (rec.hasText)
			{
				const uint8_t* t = b.Take(tl);
				if (b.Ok()) rec.text.assign((const char*)t, tl);
			}
			break;
		}
		case GC_JOURNAL_CHECK: rec.digest = b.U64(); break;
		default:
			why = "unknown record type";
			return false;
		}
		if (!b.Ok() || !b.AtEnd())
		{
			why = "malformed record";
			return false;
		}
		recs.push_back(rec);
	}
	if (!r.AtEnd())
	{
		why = "bytes after the last record";
		return false;
	}
	return true;
}

void EncodeNet(const std::vector<RtNetMessage>& msgs, std::vector<uint8_t>& out)
{
	out.clear();
	RtByteWriter w(out);
	w.Bytes(kNetMagic, 4);
	w.U16(kFormatVersion);
	w.U16(0);
	w.U32((uint32_t)msgs.size());
	for (size_t i = 0; i < msgs.size(); ++i)
	{
		std::vector<uint8_t> body;
		RtByteWriter b(body);
		WriteNetBody(b, msgs[i]);
		w.U16((uint32_t)body.size());
		w.Bytes(&body[0], body.size());
	}
}

bool DecodeNet(const uint8_t* p, size_t n, std::vector<RtNetMessage>& msgs, std::string& why)
{
	msgs.clear();
	RtByteReader r(p, n);
	uint32_t count = 0;
	if (!ReadHeader(r, kNetMagic, count, why)) return false;
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t len = r.U16();
		const uint8_t* body = r.Take(len);
		if (!r.Ok())
		{
			why = "truncated message";
			return false;
		}
		RtByteReader b(body, len);
		RtNetMessage m;
		if (!ReadNetBody(b, m) || !b.AtEnd())
		{
			why = "malformed message or unknown message id";
			return false;
		}
		msgs.push_back(m);
	}
	if (!r.AtEnd())
	{
		why = "bytes after the last message";
		return false;
	}
	return true;
}

bool NetMessageFromUi(int id)
{
	switch (id)
	{
	case kRtNet_None:
	case kRtNet_sendTurnComplete:
	case kRtNet_sendTurnUnready:
	case kRtNet_SendAICivsProcessed:
	case kRtNet_sendAutoMoves:
		return false;
	default: return id > kRtNet_None && id < kRtNet_Count;
	}
}

const char* NetMessageName(int id) { return id >= 0 && id < kRtNet_Count ? kNetNames[id] : "?"; }

}  // namespace lekmod_portable
