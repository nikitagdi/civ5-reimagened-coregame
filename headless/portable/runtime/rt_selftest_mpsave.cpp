// Scenario `mpsave` (task N3.13; run.sh --scenario mpsave --humans 3 --ai 1 --save-at 30 --continue 20): the save and the load of a network game with several
// humans, one phase per process (the core is a process singleton), the humans played by test bots (setup "autoplay_humans"), through the C ABI only.
//  - phase "save": new game; at game turn 12 human 2 is kicked (an AI plays the slot), at 15 a new human takes the original AI slot with the nick "newbie", so the
//    save holds slot owners and nicks that are not the setup's; gc_pump to game turn --save-at; a gc_save right after a command is REJECTED (not a safe point); at the first safe
//    point gc_save: the bytes are the same twice and the digests do not move; the digests of every viewer, the Q_GAME and Q_TURN picture and the state digest are written next to
//    the save; then --continue more turns with the digests of every turn;
//  - phase "load": on a fresh instance, broken saves are GC_ERR_BAD_DATA (the instance keeps its slot), then gc_load_game of the save: right after it every viewer's digest, the
//    state digest and the Q_GAME / Q_TURN picture (nicks, slot owners, kicks, turn flags; not the connection) equal the saved ones; it is a safe point, nothing is queued and a gc_save there is the loaded save byte for byte; the host's CONNECT for
//    every human puts them on line without changing the digests; the active player is the server seat; --continue turns: the digests of every turn are compared with the
//    save phase's (a check with --strict 1; before the C1 flags of N3.15 the first difference is reported only).
// "mpsave-turn|phase|turn|digests..." lines give the per-turn digests of both phases.
#include "CvGameCoreDLLPCH.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <map>
#include <string>
#include <vector>

#include "../abi/gc_internal.h"
#include "rt_active.h"
#include "rt_autoplay.h"
#include "rt_save.h"
#include "rt_selftest.h"
#include "rt_selftest_slots.h"
#include "rt_session.h"

using namespace lekmod_portable;

namespace {

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

std::string Escape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		const char ch = s[i];
		if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
		else if ((unsigned char)ch >= 0x20) out += ch;
	}
	return out;
}

std::string Num(long long v) { return Format("%lld", v); }

struct Check
{
	std::string json;
	int failed;
	Check() : failed(0) {}
	void add(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 4, 5)))
	{
		char detail[2048];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
		json += std::string(json.empty() ? "" : ",") + "{\"check\":\"" + name + "\",\"ok\":" + (ok ? "true" : "false") + ",\"detail\":\"" + Escape(detail) + "\"}";
		printf("mpsave: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
		if (!ok) ++failed;
	}
};

double NowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct Seen
{
	const gc_host* outer;
	int logErrors, logWarnings, luaErrors;
	int slotEvents;
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0), slotEvents(0) {}
};

void OnLog(void* user, int32_t level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == GC_LOG_ERROR) { ++s->logErrors; if (s->lines.size() < 20) s->lines.push_back(std::string("log error: ") + msg); }
	if (level == GC_LOG_WARN) ++s->logWarnings;
	if (s->outer && s->outer->log) s->outer->log(s->outer->user, level, msg);
}

int32_t OnReadFile(void* user, const char* path, gc_buf* out)
{
	Seen* s = static_cast<Seen*>(user);
	return s->outer->read_file(s->outer->user, path, out);
}

void OnEvent(void* user, const gc_event* ev)
{
	Seen* s = static_cast<Seen*>(user);
	if (ev->kind == GC_EVENT_SLOT_CHANGED) ++s->slotEvents;
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 20) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
}

gc_command Cmd(int kind, int player)
{
	gc_command c;
	memset(&c, 0, sizeof(c));
	c.kind = (uint16_t)kind;
	c.player = (uint8_t)player;
	return c;
}

std::string Hex(uint64_t v) { return Format("%016llx", (unsigned long long)v); }

uint64_t Digest(gc_instance* inst, int viewer)
{
	gc_error err;
	uint64_t d = 0;
	if (gc_state_digest(inst, viewer, &d, &err) != GC_OK) return 0;
	return d;
}

// -1 and the team of every living major civ, ascending.
std::vector<int> Viewers()
{
	std::vector<int> v;
	v.push_back(-1);
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		const CvPlayer& p = GET_PLAYER((PlayerTypes)i);
		if (!p.isAlive()) continue;
		const int t = (int)p.getTeam();
		bool have = false;
		for (size_t k = 0; k < v.size(); ++k) have = have || v[k] == t;
		if (!have) v.push_back(t);
	}
	return v;
}

bool WriteFile(const std::string& path, const void* data, size_t n)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) return false;
	const bool ok = n == 0 || fwrite(data, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
	out.clear();
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
	fclose(f);
	return true;
}

void Reseal(std::vector<uint8_t>& b)
{
	if (b.size() < kGcsFixedHeader) return;
	const uint64_t h = Xxh64(&b[kGcsFixedHeader], b.size() - kGcsFixedHeader, 0);
	for (int i = 0; i < 8; ++i) b[24 + i] = (uint8_t)(h >> (8 * i));
}

bool GetGame(gc_instance* inst, gcq_game* out)
{
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const int32_t rc = gc_query(inst, GC_Q_GAME, NULL, 0, &b, &err);
	const bool ok = rc == GC_OK && b.len >= sizeof(gcq_header) + sizeof(gcq_game);
	if (ok) memcpy(out, b.data + sizeof(gcq_header), sizeof(*out));
	gc_buf_release(&b);
	return ok;
}

// The Q_GAME and Q_TURN picture as text, one line per fact; the connection flags are left out (they are not saved). Spaces in a nick become '_'.
std::string Picture(gc_instance* inst)
{
	std::string s;
	gcq_game g;
	if (GetGame(inst, &g))
		s += Format("game turn %d phase %d humans %d humans_left %d turn_mode %d game_type %d blocker %d safe %d\n", (int)g.turn, (int)g.phase, (int)g.humans, (int)g.humans_left, (int)g.turn_mode, (int)g.game_type,
			(int)g.blocker, (int)g.safe_point);
	gc_buf b = {0, 0, 0, 0};
	gc_error err;
	memset(&err, 0, sizeof(err));
	if (gc_query(inst, GC_Q_TURN, NULL, 0, &b, &err) == GC_OK && b.len >= sizeof(gcq_header))
	{
		gcq_header h;
		memcpy(&h, b.data, sizeof(h));
		for (uint32_t k = 0; k < h.count && h.record_size >= sizeof(gcq_turn); ++k)
		{
			gcq_turn r;
			memcpy(&r, b.data + sizeof(h) + (size_t)k * h.record_size, sizeof(r));
			std::string nick = r.nick != GCQ_NO_STRING && r.nick < h.strings_size ? std::string((const char*)b.data + h.strings_offset + r.nick) : std::string("-");
			for (size_t i = 0; i < nick.size(); ++i) if (nick[i] == ' ') nick[i] = '_';
			const unsigned flags = r.flags & ~(unsigned)(GC_TURN_F_CONNECTED | GC_TURN_F_DISCONNECTED);
			s += Format("turnrec %d team %d slot %d flags %04x order %d blocker %d nick %s\n", (int)r.player, (int)r.team, (int)r.slot_status, flags, (int)r.turn_order, (int)r.blocker, nick.c_str());
		}
	}
	gc_buf_release(&b);
	return s;
}

struct Driver
{
	gc_instance* inst;
	int seat;
	int activeBroken, stuck;
	long long calls;
	std::string stop;
	gc_pump_result pr;
	gc_error err;

	Driver() : inst(NULL), seat(-1), activeBroken(0), stuck(0), calls(0) { memset(&pr, 0, sizeof(pr)); memset(&err, 0, sizeof(err)); }

	bool Pump()
	{
		++calls;
		const int32_t rc = gc_pump(inst, 64, &pr, &err);
		if (CvPreGame::activePlayer() != seat) ++activeBroken;
		if (pr.status == GC_PUMP_STUCK) { ++stuck; stop = Format("STUCK at turn %d: %s", pr.turn, err.message); return false; }
		if (pr.status == GC_PUMP_GAME_OVER) { stop = Format("GAME_OVER at turn %d", pr.turn); return false; }
		if (rc != GC_OK) { stop = Format("gc_pump at turn %d: %d %s", pr.turn, rc, err.message); return false; }
		return true;
	}

	// gc_pump until the game turn is `turn` or later and the game is at a safe point (the start of that turn's human phase).
	bool ToSafePoint(int turn)
	{
		for (int i = 0; i < 40000; ++i)
		{
			gcq_game g;
			if (GC.getGame().getGameTurn() >= turn && GetGame(inst, &g) && g.safe_point == 1 && g.phase == GC_PHASE_HUMANS) return true;
			if (!Pump()) return false;
		}
		stop = Format("no safe point at turn %d after 40000 gc_pump calls", turn);
		return false;
	}

	// gc_pump until the game turn reaches `target`; per turn change the digests of -1 and of every human team, and the coarse state digest, as one text.
	bool PlayTurns(int target, const char* phase, const std::vector<int>& humanTeams, std::vector<std::pair<int, std::string> >& out)
	{
		int turn = GC.getGame().getGameTurn();
		for (long long i = 0; turn < target; ++i)
		{
			if (i > 40000LL * (target - turn + 1)) { stop = Format("turn %d: no progress towards turn %d", turn, target); return false; }
			if (!Pump()) return false;
			if (pr.turn != turn)
			{
				turn = pr.turn;
				std::string line = Hex(Digest(inst, -1));
				for (size_t k = 0; k < humanTeams.size(); ++k) line += " " + Hex(Digest(inst, humanTeams[k]));
				line += " " + Hex(RtSession::StateDigest());
				printf("mpsave-turn|%s|%d|%s\n", phase, turn, line.c_str());
				out.push_back(std::make_pair(turn, line));
			}
		}
		return true;
	}
};

// What the save phase leaves for the load phase next to the save.
struct Saved
{
	int saveTurn;
	std::vector<int> humanTeams;
	std::map<int, std::string> view;                 // viewer -> digest right after the save
	std::string state;                               // RtSession::StateDigest
	std::string full;                                // gc_state_digest(-2): the whole truth plus the serialized state
	std::string picture;                            // Picture()
	std::vector<std::pair<int, std::string> > turns; // per turn after the save
	Saved() : saveTurn(-1) {}

	std::string Text() const
	{
		std::string s = Format("save_turn %d\nstate %s\nfull %s\n", saveTurn, state.c_str(), full.c_str());
		for (size_t i = 0; i < humanTeams.size(); ++i) s += Format("human_team %d\n", humanTeams[i]);
		for (std::map<int, std::string>::const_iterator it = view.begin(); it != view.end(); ++it) s += Format("view %d %s\n", it->first, it->second.c_str());
		for (size_t i = 0; i < turns.size(); ++i) s += Format("turn %d %s\n", turns[i].first, turns[i].second.c_str());
		s += "picture\n" + picture;
		return s;
	}
	bool Parse(const std::string& text)
	{
		size_t pos = 0;
		bool inPicture = false;
		while (pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) end = text.size();
			const std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			if (inPicture) { picture += line + "\n"; continue; }
			if (line == "picture") { inPicture = true; continue; }
			char a[256];
			int n = 0;
			if (sscanf(line.c_str(), "save_turn %d", &n) == 1) saveTurn = n;
			else if (sscanf(line.c_str(), "state %255s", a) == 1) state = a;
			else if (sscanf(line.c_str(), "full %255s", a) == 1) full = a;
			else if (sscanf(line.c_str(), "human_team %d", &n) == 1) humanTeams.push_back(n);
			else if (sscanf(line.c_str(), "view %d %255s", &n, a) == 2) view[n] = a;
			else if (line.compare(0, 5, "turn ") == 0)
			{
				const size_t sp = line.find(' ', 5);
				if (sp != std::string::npos) turns.push_back(std::make_pair(atoi(line.c_str() + 5), line.substr(sp + 1)));
			}
		}
		return saveTurn >= 0 && !view.empty();
	}
};

}  // namespace

extern "C" int rt_selftest_mpsave(const gc_host* host, const char* phase, const char* save_file, unsigned seed, unsigned humans, unsigned ai, unsigned save_at, unsigned cont, unsigned strict,
	const char* world_size, char* report, size_t report_len)
{
	Check c;
	Seen seen;
	seen.outer = host;
	gc_host mine = {&seen, OnLog, OnReadFile, OnEvent};
	gc_error err;
	memset(&err, 0, sizeof(err));
	const bool savePhase = phase && strcmp(phase, "save") == 0;
	const bool loadPhase = phase && strcmp(phase, "load") == 0;
	const std::string file = save_file ? save_file : "";
	const std::string digestFile = file + ".digests";
	if (humans < 3) humans = 3;
	if (humans > 8) humans = 8;
	if (ai < 1) ai = 1;
	const double t0 = NowMs();
	int saveTurn = -1, endTurn = -1, turnsCompared = 0, firstDiverged = -1;
	long long saveBytes = 0;
	double msSave = 0, msLoad = 0;
	std::string firstDiff;
	Driver d;

	if ((!savePhase && !loadPhase) || file.empty()) c.add("args", false, "phase must be save or load and save_file is required (phase '%s')", phase ? phase : "");
	gc_instance* inst = c.failed ? NULL : gc_create(&mine, &err);
	if (!c.failed) c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);
	d.inst = inst;
	RtSession* session = InstanceSession(inst);

	if (inst && savePhase)
	{
		std::string players, bots;
		for (unsigned i = 0; i < humans; ++i)
		{
			players += Format("%s{\"kind\":\"human\",\"civ\":\"%s\",\"nick\":\"bot%u\"}", i ? "," : "", i ? "random" : "CIVILIZATION_ROME", i);
			bots += Format("%s%u", i ? "," : "", i);
		}
		for (unsigned i = 0; i < ai; ++i) players += ",{\"kind\":\"ai\",\"civ\":\"random\"}";
		const std::string setup = Format("{\"seed\":%u,\"game_type\":\"network\",\"players\":[%s],\"turn_mode\":\"simultaneous\",\"autoplay_humans\":[%s],\"handicap\":\"HANDICAP_CHIEFTAIN\"%s%s%s}", seed,
			players.c_str(), bots.c_str(), world_size && *world_size ? ",\"world_size\":\"" : "", world_size && *world_size ? world_size : "", world_size && *world_size ? "\"" : "");
		int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s", setup.c_str(), rc, err.message);
		if (rc == GC_OK && session)
		{
			d.seat = (int)ServerSeat();
			std::vector<int> humanList;
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (IsSeatedHuman(i)) humanList.push_back(i);
			const int h2 = humanList.size() > 2 ? humanList[2] : -1;
			const int firstAi = (int)humans;
			gc_buf buf = {0, 0, 0, 0};
			rc = gc_save(inst, &buf, &err);
			c.add("save_refused_before_pump", rc == GC_ERR_REJECTED, "gc_save before the first gc_pump: %d %s", rc, err.message);
			gc_buf_release(&buf);

			// Slot owners and nicks that are not the setup's: a kicked human, a new human on the original AI slot.
			if (d.ToSafePoint(12))
			{
				rc = gc_slot_control(inst, h2, GC_SLOT_KICK, NULL, &err);
				c.add("kick_before_save", rc == GC_OK && CvPreGame::slotStatus((PlayerTypes)h2) == SS_COMPUTER, "turn %d: KICK of %d rc %d %s", GC.getGame().getGameTurn(), h2, rc, err.message);
			}
			if (d.stop.empty() && d.ToSafePoint(15))
			{
				rc = gc_slot_control(inst, firstAi, GC_SLOT_CONNECT, "newbie", &err);
				c.add("claim_before_save", rc == GC_OK && CvPreGame::slotStatus((PlayerTypes)firstAi) == SS_TAKEN, "turn %d: CONNECT of %d as 'newbie' rc %d %s", GC.getGame().getGameTurn(), firstAi, rc, err.message);
			}
			std::vector<std::pair<int, std::string> > ignored;
			std::vector<int> humanTeams;
			for (int i = 0; i < MAX_PLAYERS; ++i)
				if (IsSeatedHuman(i) || i == h2) humanTeams.push_back((int)GET_PLAYER((PlayerTypes)i).getTeam());
			if (d.stop.empty() && d.ToSafePoint((int)save_at))
			{
				// A command takes the safe point away: no save until the next gc_pump.
				gc_command cc = Cmd(GC_CMD_CANCEL_END_TURN, humanList[0]);
				const int32_t crc = gc_apply_command(inst, &cc, &err);
				rc = gc_save(inst, &buf, &err);
				const int32_t refused = rc;
				const int refusedReason = (int)err.reason;
				const std::string refusedMsg = err.message;
				gc_buf_release(&buf);
				c.add("save_refused_after_command", crc == GC_OK && refused == GC_ERR_REJECTED, "gc_save after a command: %d reason %d %s", refused, refusedReason, refusedMsg.c_str());
				d.ToSafePoint(GC.getGame().getGameTurn());
			}
			std::vector<uint8_t> bytes;
			Saved sv;
			if (d.stop.empty())
			{
				gc_buf b = {0, 0, 0, 0};
				const double ts = NowMs();
				rc = gc_save(inst, &b, &err);
				msSave = NowMs() - ts;
				if (rc == GC_OK) bytes.assign(b.data, b.data + b.len);
				gc_buf_release(&b);
				saveTurn = GC.getGame().getGameTurn();
				saveBytes = (long long)bytes.size();
				c.add("saved", rc == GC_OK && !bytes.empty(), "turn %d: %lld bytes in %.1f ms; gc_save: %d %s", saveTurn, saveBytes, msSave, rc, err.message);
			}
			if (!bytes.empty())
			{
				sv.saveTurn = saveTurn;
				sv.humanTeams = humanTeams;
				const std::vector<int> viewers = Viewers();
				for (size_t i = 0; i < viewers.size(); ++i) sv.view[viewers[i]] = Hex(Digest(inst, viewers[i]));
				sv.state = Hex(RtSession::StateDigest());
				sv.full = Hex(Digest(inst, -2));
				sv.picture = Picture(inst);
				// The save leaves the game alone and is reproducible: a second save is byte-identical, and so are the digests.
				gc_buf b2 = {0, 0, 0, 0};
				rc = gc_save(inst, &b2, &err);
				const bool same = rc == GC_OK && b2.len == bytes.size() && memcmp(b2.data, &bytes[0], bytes.size()) == 0;
				gc_buf_release(&b2);
				bool digestsSame = Hex(RtSession::StateDigest()) == sv.state && Picture(inst) == sv.picture;
				for (size_t i = 0; i < viewers.size(); ++i) digestsSame = digestsSame && sv.view[viewers[i]] == Hex(Digest(inst, viewers[i]));
				c.add("save_stable", same && digestsSame, "second gc_save: %d, bytes identical %d, digests and Q_GAME / Q_TURN unchanged %d (%d viewer(s))", rc, same ? 1 : 0, digestsSame ? 1 : 0, (int)viewers.size());
				rc = gc_load_game(inst, &bytes[0], (uint32_t)bytes.size(), &err);
				c.add("load_on_running_instance", rc == GC_ERR_ALREADY_EXISTS, "gc_load_game on the instance that runs the game: %d %s", rc, err.message);
				c.add("write_save", WriteFile(file, &bytes[0], bytes.size()), "%s (%lld bytes)", file.c_str(), saveBytes);
				printf("mpsave-state|save|%d|state %s|rng %u\n", saveTurn, sv.state.c_str(), (unsigned)GC.getGame().getJonRand().getCallCount());
				printf("%s", ("mpsave-picture|save\n" + sv.picture).c_str());
				d.PlayTurns(saveTurn + (int)cont, "save", humanTeams, sv.turns);
				endTurn = GC.getGame().getGameTurn();
				c.add("continued", d.stop.empty(), "turn %d -> %d%s%s", saveTurn, endTurn, d.stop.empty() ? "" : ": ", d.stop.c_str());
				const std::string text = sv.Text();
				c.add("write_digests", WriteFile(digestFile, text.data(), text.size()), "%s", digestFile.c_str());
			}
			c.add("active_rule", d.activeBroken == 0, "%d time(s) the active player was not the server seat %d after a gc_pump", d.activeBroken, d.seat);
		}
	}

	if (inst && loadPhase)
	{
		std::vector<uint8_t> bytes, dtext;
		Saved want;
		const bool haveSave = ReadFile(file, bytes) && !bytes.empty();
		const bool haveDigests = ReadFile(digestFile, dtext) && want.Parse(std::string(dtext.begin(), dtext.end()));
		saveBytes = (long long)bytes.size();
		c.add("read_save", haveSave && haveDigests, "%s: %lld bytes; digests %s: %s", file.c_str(), saveBytes, digestFile.c_str(), haveDigests ? "ok" : "missing or malformed");
		if (haveSave && haveDigests)
		{
			// Broken saves: GC_ERR_BAD_DATA each, and the instance keeps its one game slot.
			struct Bad { const char* name; std::vector<uint8_t> b; const char* expect; };
			std::vector<Bad> bad;
			{
				Bad x = {"truncated", std::vector<uint8_t>(bytes.begin(), bytes.begin() + bytes.size() / 2), "truncated"};
				bad.push_back(x);
			}
			{
				Bad x = {"flipped_byte", bytes, "checksum"};
				x.b[bytes.size() / 3] ^= 0x5a;
				bad.push_back(x);
			}
			{
				// A save of the B1 format (version 1: no network turn model, one test bot): this core does not read it.
				Bad x = {"format_version_1", bytes, "version"};
				x.b[4] = 1;
				bad.push_back(x);
			}
			{
				Bad x = {"build_id", bytes, "another gamecore build"};
				const char* key = "build_id=lekmod-portable/";
				const size_t klen = strlen(key);
				for (size_t i = kGcsFixedHeader; i + klen < x.b.size() && i < 4096; ++i)
					if (memcmp(&x.b[i], key, klen) == 0) { x.b[i + klen] ^= 0x20; break; }
				Reseal(x.b);
				bad.push_back(x);
			}
			for (size_t i = 0; i < bad.size(); ++i)
			{
				const int32_t rc = gc_load_game(inst, &bad[i].b[0], (uint32_t)bad[i].b.size(), &err);
				c.add((std::string("bad_") + bad[i].name).c_str(), rc == GC_ERR_BAD_DATA && strstr(err.message, bad[i].expect) != NULL, "%u bytes: %d %s", (unsigned)bad[i].b.size(), rc, err.message);
			}
			gc_pump_result pr;
			int32_t rc = gc_pump(inst, 1, &pr, &err);
			c.add("bad_left_no_game", rc == GC_ERR_NOT_STARTED, "gc_pump after the refused loads: %d %s", rc, err.message);

			const double tl = NowMs();
			rc = gc_load_game(inst, &bytes[0], (uint32_t)bytes.size(), &err);
			msLoad = NowMs() - tl;
			c.add("loaded", rc == GC_OK, "gc_load_game: %d %s (%.1f ms)", rc, err.message, msLoad);
			if (rc == GC_OK)
			{
				session = InstanceSession(inst);
				d.seat = (int)ServerSeat();
				saveTurn = GC.getGame().getGameTurn();
				std::string diff, vline;
				int differ = 0;
				for (std::map<int, std::string>::const_iterator it = want.view.begin(); it != want.view.end(); ++it)
				{
					const std::string got = Hex(Digest(inst, it->first));
					vline += Format(" %d:%s", it->first, got.c_str());
					if (got != it->second) { ++differ; diff += Format(" viewer %d: %s, saved %s;", it->first, got.c_str(), it->second.c_str()); }
				}
				printf("mpsave-view|load|%d|%s\n", saveTurn, vline.c_str());
				const std::string state = Hex(RtSession::StateDigest());
				c.add("digests_after_load", saveTurn == want.saveTurn && differ == 0 && state == want.state, "turn %d (saved at %d): %d/%d viewer digest(s) equal%s; state digest %s (saved %s)", saveTurn, want.saveTurn,
					(int)want.view.size() - differ, (int)want.view.size(), diff.c_str(), state.c_str(), want.state.c_str());
				const std::string pic = Picture(inst);
				printf("mpsave-picture|load\n%s", pic.c_str());
				c.add("picture_after_load", pic == want.picture, "Q_GAME and Q_TURN (humans, nicks, slot owners, kicks, turn flags) %s the saved ones", pic == want.picture ? "equal" : "DIFFER from");
				if (pic != want.picture) printf("mpsave-picture-saved\n%s", want.picture.c_str());
				c.add("active_player_is_seat", CvPreGame::activePlayer() == ServerSeat() && d.seat >= 0, "the active player is %d, the server seat %d", (int)CvPreGame::activePlayer(), d.seat);
				gcq_game g;
				const bool gotG = GetGame(inst, &g);
				c.add("safe_point_after_load", gotG && g.safe_point == 1 && g.phase == GC_PHASE_HUMANS && g.humans == session->SeatedHumans(), "Q_GAME: safe_point %d unsafe_reason %d phase %d humans %d humans_left %d", gotG ? (int)g.safe_point : -1,
					gotG ? (int)g.unsafe_reason : -1, gotG ? (int)g.phase : -1, gotG ? (int)g.humans : -1, gotG ? (int)g.humans_left : -1);
				{
					// Nothing the load path does is left over (seed 2 x 4 humans: the AI-processed stamp of the load queued a second SendAICivsProcessed; no human waited while it was
					// queued, so the first gc_pump gave no bot round and the loaded game played a different order from turn 25 on): no net message is queued, and a save
					// right after the load, before the humans come back, is the loaded save byte for byte (every DLL object, the runtime and the ABI state).
					const int queued = session->Engine().NetQueueSize();
					gc_buf b = {0, 0, 0, 0};
					const int32_t rs = gc_save(inst, &b, &err);
					const bool same = rs == GC_OK && b.len == bytes.size() && memcmp(b.data, &bytes[0], bytes.size()) == 0;
					c.add("resave_after_load", queued == 0 && same, "%d net message(s) queued; gc_save: %d %s, %lld bytes, byte-identical to the loaded save: %d", queued, rs, err.message, (long long)b.len, same ? 1 : 0);
					gc_buf_release(&b);
				}

				// The host's CONNECT for every human: nobody is on line after a load; then everybody is, and nothing else changed.
				int off = 0, on = 0, connectBad = 0;
				std::vector<int> humanList;
				for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
					if (IsSeatedHuman(i)) humanList.push_back(i);
				for (size_t h = 0; h < humanList.size(); ++h)
					if (!session->Engine().SlotConnected((PlayerTypes)humanList[h])) ++off;
				for (size_t h = 0; h < humanList.size(); ++h)
				{
					const int32_t crc = gc_slot_control(inst, humanList[h], GC_SLOT_CONNECT, NULL, &err);
					if (crc != GC_OK) { ++connectBad; printf("mpsave-connect|player %d rc %d %s\n", humanList[h], crc, err.message); }
					if (session->Engine().SlotConnected((PlayerTypes)humanList[h])) ++on;
				}
				int differ2 = 0;
				for (std::map<int, std::string>::const_iterator it = want.view.begin(); it != want.view.end(); ++it)
					if (Hex(Digest(inst, it->first)) != it->second) ++differ2;
				c.add("connect_after_load", off == (int)humanList.size() && on == (int)humanList.size() && connectBad == 0 && differ2 == 0 && Hex(RtSession::StateDigest()) == want.state && seen.slotEvents == (int)humanList.size(),
					"%d human(s): %d off line after the load, %d on line after CONNECT (%d refused), %d viewer digest(s) changed, %d SLOT_CHANGED event(s)", (int)humanList.size(), off, on, connectBad, differ2, seen.slotEvents);
				c.add("active_player_after_connect", CvPreGame::activePlayer() == ServerSeat(), "the active player is %d, the server seat %d", (int)CvPreGame::activePlayer(), d.seat);

				// The CONNECTs of a loaded game do not run CvPlayer::reconnected (it would give the server seat a PLAYER_CONNECTING notification per human): the full serialized
				// digest (gc_state_digest(-2)) is the saved one, and so is the save, byte for byte. Strict (--strict-continuation 1) requires both; otherwise they are reported.
				{
					const std::string full = Hex(Digest(inst, -2));
					gc_buf b = {0, 0, 0, 0};
					const int32_t rs = gc_save(inst, &b, &err);
					const bool same = rs == GC_OK && b.len == bytes.size() && memcmp(b.data, &bytes[0], bytes.size()) == 0;
					gc_buf_release(&b);
					const bool fullSame = !want.full.empty() && full == want.full;
					const std::string what = Format("full digest %s (saved %s); gc_save after the CONNECTs: rc %d, byte-identical to the loaded save: %d", full.c_str(), want.full.c_str(), rs, same ? 1 : 0);
					if (strict) c.add("full_digest_after_connect", fullSame && same, "%s", what.c_str());
					else printf("mpsave: %-4s full_digest_after_connect (not strict): %s\n", (fullSame && same) ? "info" : "WARN", what.c_str());
				}
				int32_t again = gc_load_game(inst, &bytes[0], (uint32_t)bytes.size(), &err);
				c.add("second_load", again == GC_ERR_ALREADY_EXISTS, "a second gc_load_game: %d %s", again, err.message);
				again = gc_new_game(inst, "{\"game_type\":\"network\",\"seed\":1}", &err);
				c.add("new_game_after_load", again == GC_ERR_ALREADY_EXISTS, "gc_new_game after the load: %d %s", again, err.message);

				// On with the game: the same turns as the saved game played.
				Saved got;
				const double tp = NowMs();
				d.PlayTurns(saveTurn + (int)cont, "load", want.humanTeams, got.turns);
				endTurn = GC.getGame().getGameTurn();
				std::string first;
				for (size_t i = 0; i < got.turns.size() && i < want.turns.size(); ++i)
				{
					++turnsCompared;
					if (got.turns[i] != want.turns[i] && firstDiverged < 0)
					{
						firstDiverged = got.turns[i].first;
						first = Format("turn %d: loaded %s, saved game %s", got.turns[i].first, got.turns[i].second.c_str(), want.turns[i].second.c_str());
					}
				}
				c.add("continued", d.stop.empty() && got.turns.size() == want.turns.size(), "turn %d -> %d, %d turn(s) (save phase %d) in %.0f ms%s%s", saveTurn, endTurn, (int)got.turns.size(), (int)want.turns.size(), NowMs() - tp,
					d.stop.empty() ? "" : ": ", d.stop.c_str());
				const bool sameRun = firstDiverged < 0 && turnsCompared == (int)want.turns.size();
				const std::string detail = Format("%d turn(s) compared (digests of -1, the human teams, and the state digest), first difference %s", turnsCompared, first.empty() ? "none" : first.c_str());
				// Strict (run.sh --strict-continuation 1): the loaded game must play on exactly like the saved one. Before the C1 flags of N3.15 the DLL's own load path does not
				// restore every input (civhost DS-0010 / DS-0019..0022), so the difference is reported only.
				if (strict) c.add("same_continuation", sameRun, "%s", detail.c_str());
				else printf("mpsave: %-4s same_continuation (not strict): %s\n", sameRun ? "info" : "WARN", detail.c_str());
				c.add("active_rule", d.activeBroken == 0, "%d time(s) the active player was not the server seat %d after a gc_pump", d.activeBroken, d.seat);
			}
		}
	}
	if (inst)
	{
		c.add("no_stuck", d.stuck == 0, "%d STUCK", d.stuck);
		c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
		c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("mpsave-event|%s\n", seen.lines[i].c_str());
	const double msTotal = NowMs() - t0;
	const std::string out = "{\"failed\":" + Num(c.failed) + ",\"phase\":\"" + Escape(phase ? phase : "") + "\",\"seed\":" + Num(seed) + ",\"humans\":" + Num(humans) + ",\"ai\":" + Num(ai) + ",\"save_at\":" + Num(save_at) +
		",\"continue\":" + Num(cont) + ",\"save_turn\":" + Num(saveTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"save_bytes\":" + Num(saveBytes) + ",\"stuck\":" + Num(d.stuck) + ",\"log_errors\":" + Num(seen.logErrors) +
		",\"lua_errors\":" + Num(seen.luaErrors) + ",\"turns_compared\":" + Num(turnsCompared) + ",\"first_diverged\":" + Num(firstDiverged) + ",\"strict\":" + Num(strict) + ",\"pump_calls\":" + Num(d.calls) +
		",\"ms\":{\"save\":" + Format("%.1f", msSave) + ",\"load\":" + Format("%.1f", msLoad) + ",\"total\":" + Format("%.1f", msTotal) + "},\"stop_reason\":\"" + Escape(d.stop) + "\",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("mpsave-summary|%s|seed %u|humans %u|ai %u|save turn %d|end turn %d|bytes %lld|save %.0f ms|load %.0f ms|turns compared %d|first diverged %d|failed %d\n", phase ? phase : "?", seed, humans, ai, saveTurn, endTurn,
		saveBytes, msSave, msLoad, turnsCompared, firstDiverged, c.failed);
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
