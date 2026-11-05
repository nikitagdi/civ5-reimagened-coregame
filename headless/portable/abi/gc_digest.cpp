// gc_state_digest: XXH64 and the canonical serialization of the viewer's queries. C++03.
#include "CvGameCoreDLLPCH.h"

#include "gc_digest.h"
#include "../runtime/rt_save.h"
#include "CvDllContext.h"
#include "gc_query.h"
#include "gc_query_city.h"
#include "../runtime/rt_active.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace lekmod_portable {

namespace {

const uint64_t kP1 = 11400714785074694791ULL;
const uint64_t kP2 = 14029467366897019727ULL;
const uint64_t kP3 = 1609587929392839161ULL;
const uint64_t kP4 = 9650029242287828579ULL;
const uint64_t kP5 = 2870177450012600261ULL;

inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

// Little-endian loads that do not depend on alignment or host byte order.
inline uint64_t Read64(const uint8_t* p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
	return v;
}
inline uint32_t Read32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

inline uint64_t Round(uint64_t acc, uint64_t input)
{
	acc += input * kP2;
	acc = Rotl(acc, 31);
	return acc * kP1;
}

inline uint64_t MergeRound(uint64_t acc, uint64_t val)
{
	acc ^= Round(0, val);
	return acc * kP1 + kP4;
}

}  // namespace

uint64_t Xxh64(const void* data, size_t len, uint64_t seed)
{
	const uint8_t* p = static_cast<const uint8_t*>(data);
	const uint8_t* const end = p + len;
	uint64_t h;
	if (len >= 32)
	{
		uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
		const uint8_t* const limit = end - 32;
		do
		{
			v1 = Round(v1, Read64(p));
			v2 = Round(v2, Read64(p + 8));
			v3 = Round(v3, Read64(p + 16));
			v4 = Round(v4, Read64(p + 24));
			p += 32;
		} while (p <= limit);
		h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
		h = MergeRound(h, v1);
		h = MergeRound(h, v2);
		h = MergeRound(h, v3);
		h = MergeRound(h, v4);
	}
	else
	{
		h = seed + kP5;
	}
	h += (uint64_t)len;
	while (p + 8 <= end)
	{
		h ^= Round(0, Read64(p));
		h = Rotl(h, 27) * kP1 + kP4;
		p += 8;
	}
	if (p + 4 <= end)
	{
		h ^= (uint64_t)Read32(p) * kP1;
		h = Rotl(h, 23) * kP2 + kP3;
		p += 4;
	}
	while (p < end)
	{
		h ^= (uint64_t)(*p) * kP5;
		h = Rotl(h, 11) * kP1;
		++p;
	}
	h ^= h >> 33;
	h *= kP2;
	h ^= h >> 29;
	h *= kP3;
	h ^= h >> 32;
	return h;
}

// The smoke test (C, no access to namespaces) checks the published test vectors through this entry.
extern "C" uint64_t lekmod_portable_xxh64(const void* data, size_t len, uint64_t seed) { return Xxh64(data, len, seed); }

namespace {

const int kSimulationPart = 0x7E;
const int kReplicaPregamePart = 0x7C;  // viewer_team -3 (B4 gc-replica)
const int kReplicaRuntimePart = 0x7D;  // digest part of viewer_team -2: one xxh64 per serialized game object

void AppendPart(std::vector<uint8_t>& stream, int kind, const gc_buf& buf)
{
	stream.push_back((uint8_t)kind);
	const uint32_t n = (uint32_t)buf.len;
	for (int i = 0; i < 4; ++i) stream.push_back((uint8_t)(n >> (8 * i)));
	if (buf.len > 0) stream.insert(stream.end(), buf.data, buf.data + buf.len);
}

// One part: runs the query, appends it, releases the reply.
int32_t Part(std::vector<uint8_t>& stream, int kind, int32_t rc, gc_buf& buf, std::string& msg, const char* name)
{
	if (rc != GC_OK)
	{
		msg = std::string("digest: ") + name + " failed: " + msg;
		return rc;
	}
	AppendPart(stream, kind, buf);
	if (buf.release) buf.release(&buf);
	return GC_OK;
}

// The players whose top bars belong to the viewer (v2): the seated humans of the viewer's team (every seated human for viewer -1; in a single game
// that is the one human, as in v1); a viewer team without a human contributes its first living major civ (v1), none at all -> empty.
void BarPlayers(int viewerTeam, std::vector<int>& out)
{
	out.clear();
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
		if (IsSeatedHuman(i) && (viewerTeam < 0 || (int)GET_PLAYER((PlayerTypes)i).getTeam() == viewerTeam)) out.push_back(i);
	if (!out.empty() || viewerTeam < 0) return;
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const CvPlayer& pl = GET_PLAYER((PlayerTypes)i);
		if (pl.isAlive() && !pl.isObserver() && (int)pl.getTeam() == viewerTeam)
		{
			out.push_back(i);
			return;
		}
	}
}

// viewer_team -2: the simulation digest for tests. One 8-byte xxh64 per serialized object, in the order of a save (game, map, every team, every player,
// the supporting class data): everything the game serializes, so the RNG state of the game and the map, the threat value of every city (DS-0035), the treasury fields
// (DS-0037), the might cache (DS-0034) and the plot caches (DS-0040, -41) are all in it, which is what a query must not touch.
FILE* g_objectDump = NULL;  // GC_REPLICA_DIGEST_DUMP: the objects of the last replica digest, for finding a divergence (u32 length + bytes each)

void DumpObject(const void* p, uint32_t n)
{
	if (!g_objectDump) return;
	fwrite(&n, 4, 1, g_objectDump);
	if (n) fwrite(p, 1, n, g_objectDump);
}

void AppendObjectHash(std::vector<uint8_t>& out, FMemoryStream& s)
{
	DumpObject(s.GetBuf(), (uint32_t)s.GetEOF());
	const uint64_t h = Xxh64(s.GetBuf(), s.GetEOF(), 0);
	for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(h >> (8 * i)));
}

void SimulationPart(std::vector<uint8_t>& stream)
{
	std::vector<uint8_t> hashes;
	{
		FMemoryStream s;
		GC.getGame().Write(s);
		AppendObjectHash(hashes, s);
	}
	{
		FMemoryStream s;
		GC.getMap().Write(s);
		AppendObjectHash(hashes, s);
	}
	for (int i = 0; i < MAX_TEAMS; ++i)
	{
		FMemoryStream s;
		GET_TEAM((TeamTypes)i).Write(s);
		AppendObjectHash(hashes, s);
	}
	for (int i = 0; i < MAX_PLAYERS; ++i)
	{
		FMemoryStream s;
		GET_PLAYER((PlayerTypes)i).Write(s);
		AppendObjectHash(hashes, s);
	}
	{
		FMemoryStream s;
		GC.getGame().WriteSupportingClassData(s);
		AppendObjectHash(hashes, s);
	}
	gc_buf part = {0, 0, 0, 0};
	part.data = &hashes[0];
	part.len = hashes.size();
	AppendPart(stream, kSimulationPart, part);
}

}  // namespace

int32_t ViewerDigest(int32_t viewerTeam, int32_t lastPumpStatus, int32_t blocking, const RtSession::BlockTarget& target, uint64_t* out, std::string& msg)
{
	const bool simulation = viewerTeam == -2;  // the whole truth plus the serialized state (SimulationPart)
	if (simulation) viewerTeam = -1;
	std::vector<uint8_t> stream;
	stream.reserve(64 * 1024);
	gc_buf buf = {0, 0, 0, 0};
	int32_t viewer[1];
	viewer[0] = viewerTeam;

	int32_t rc = RunGameQuery(GC_Q_GAME, NULL, 0, lastPumpStatus, blocking, target, &buf, msg);
	if (Part(stream, GC_Q_GAME, rc, buf, msg, "Q_GAME") != GC_OK) return rc;
	rc = RunGameQuery(GC_Q_PLOTS, viewer, 1, lastPumpStatus, blocking, target, &buf, msg);
	if (Part(stream, GC_Q_PLOTS, rc, buf, msg, "Q_PLOTS") != GC_OK) return rc;
	rc = RunGameQuery(GC_Q_UNITS, viewer, 1, lastPumpStatus, blocking, target, &buf, msg);
	if (Part(stream, GC_Q_UNITS, rc, buf, msg, "Q_UNITS") != GC_OK) return rc;
	rc = RunGameQuery(GC_Q_CITIES, viewer, 1, lastPumpStatus, blocking, target, &buf, msg);
	if (Part(stream, GC_Q_CITIES, rc, buf, msg, "Q_CITIES") != GC_OK) return rc;
	std::vector<int> bars;
	BarPlayers(viewerTeam, bars);
	if (!bars.empty())
	{
		for (size_t i = 0; i < bars.size(); ++i)
		{
			// The top bar of a human is read as that human (the getters with a local-player branch).
			ActiveAs as((PlayerTypes)bars[i], IsSeatedHuman(bars[i]));
			rc = BuildTopBar(bars[i], &buf, msg);
			if (Part(stream, GC_Q_TOP_BAR, rc, buf, msg, "Q_TOP_BAR") != GC_OK) return rc;
		}
	}
	else
	{
		const gc_buf empty = {0, 0, 0, 0};
		AppendPart(stream, GC_Q_TOP_BAR, empty);
	}
	if (simulation) SimulationPart(stream);
	*out = Xxh64(&stream[0], stream.size(), 0);
	return GC_OK;
}

int32_t ReplicaDigest(const RtSession& session, uint64_t* out, std::string& msg)
{
	if (!session.InGame())
	{
		msg = "no game is running";
		return GC_ERR_NOT_STARTED;
	}
	// Pure game state, no ABI-layer input (a replica's last pump status or bots are not the server's): the pregame (slots), every serialized
	// game object as in a save, and the runtime section a save would carry (loopback handshake, virtual clock, Lua RNG, auto-choice state).
	std::vector<uint8_t> stream;
	const char* dumpPath = getenv("GC_REPLICA_DIGEST_DUMP");
	g_objectDump = dumpPath && *dumpPath ? fopen(dumpPath, "wb") : NULL;

	{
		std::auto_ptr<ICvPreGame1> pre(const_cast<RtSession&>(session).Context()->GetPreGame());
		FMemoryStream s;
		pre->write(s);
		std::vector<uint8_t> h;
		AppendObjectHash(h, s);
		gc_buf part = {&h[0], h.size(), 0, 0};
		AppendPart(stream, kReplicaPregamePart, part);
	}
	SimulationPart(stream);
	{
		std::vector<uint8_t> rt;
		WriteRuntimeState(session, rt);
		// The slots on line (and off line only by a load) are inputs of the rules a save does not keep in the game: in the digest so a replica that misses them shows.
		const RtEngine& engine = const_cast<RtSession&>(session).Engine();
		for (int i = 0; i < MAX_PLAYERS; ++i) rt.push_back((uint8_t)((engine.SlotConnected((PlayerTypes)i) ? 1 : 0) | (engine.SlotOfflineByLoad((PlayerTypes)i) ? 2 : 0)));
		DumpObject(rt.empty() ? NULL : &rt[0], (uint32_t)rt.size());
		if (g_objectDump) fclose(g_objectDump);
		g_objectDump = NULL;
		const uint64_t hv = Xxh64(rt.empty() ? NULL : &rt[0], rt.size(), 0);
		uint8_t h[8];
		for (int i = 0; i < 8; ++i) h[i] = (uint8_t)(hv >> (8 * i));
		gc_buf part = {h, 8, 0, 0};
		AppendPart(stream, kReplicaRuntimePart, part);
	}
	*out = Xxh64(&stream[0], stream.size(), 0);
	return GC_OK;
}

}  // namespace lekmod_portable
