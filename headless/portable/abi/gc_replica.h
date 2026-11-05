// B4 gc-replica: the input journal of a server, its replay on a replica, and the net messages of a replica's UI.
// The byte formats (GCJ1, GCN1) are the contract of gamecore_abi.h; this file holds their codec and the journal of the process. C++03.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "../runtime/rt_net.h"
#include "gamecore_abi.h"

namespace lekmod_portable {

struct JournalRecord
{
	int type;    // GC_JOURNAL_*
	int player;  // the human the input is for (COMMAND, NET, SLOT); -1 otherwise
	// COMMAND: kind, args, text. NET: net (id, args, text). PUMP: slices, status, turn. SLOT: op, text (nick), hasText. CHECK: digest.
	int kind;
	std::vector<int32_t> args;
	std::string text;
	bool hasText;
	RtNetMessage net;
	uint32_t slices;
	int32_t status;
	int32_t turn;
	int32_t op;
	uint64_t digest;
	JournalRecord() : type(0), player(-1), kind(0), hasText(false), slices(0), status(0), turn(0), op(0), digest(0) {}
};

// The journal of this process (one gc_instance per process): records are appended only while it is on (GC_ROLE_SERVER).
void JournalEnable(bool on);
bool JournalOn();
void JournalAppend(const JournalRecord& r);
size_t JournalSize();
// Moves the records out as GCJ1 bytes; the journal is empty afterwards.
void JournalTake(std::vector<uint8_t>& out);

void EncodeJournal(const std::vector<JournalRecord>& recs, std::vector<uint8_t>& out);
bool DecodeJournal(const uint8_t* p, size_t n, std::vector<JournalRecord>& recs, std::string& why);

// GCN1: a list of net messages (the sender is not in it: the server takes it from the connection's seat).
void EncodeNet(const std::vector<RtNetMessage>& msgs, std::vector<uint8_t>& out);
bool DecodeNet(const uint8_t* p, size_t n, std::vector<RtNetMessage>& msgs, std::string& why);

// Net messages a human's UI may send through gc_net_submit: the gamecore's own gDLL->send* calls of a UI action. The turn handshake
// (turn complete / unready, AI civs processed) is not among them: the end of turn is GC_CMD_END_TURN, checked by the server.
bool NetMessageFromUi(int id);
const char* NetMessageName(int id);

}  // namespace lekmod_portable
