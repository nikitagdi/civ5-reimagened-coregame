// Scenario `saveload` (task G4a, milestone M5; run.sh --scenario saveload --save-at T --continue N --seeds S,.. --ai A,..). Two processes per
// seed x AI count (one game per process): phase "save" plays with the test bot to game turn T, saves at the first save point (gc_save), writes
// the save and the digests it saw, and plays N more turns; phase "load" loads the save in a fresh instance and must see the same digests,
// right after the load (every viewer) and after every one of the N turns (viewer -1 and the human's team). See rt_selftest.h.
// "saveload-turn|phase|turn|digest -1|digest human" lines give the per-turn digests of both phases.
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
#include "rt_autoplay.h"
#include "rt_diplo.h"
#include "rt_lua.h"
#include "rt_save.h"
#include "rt_selftest.h"
#include "rt_session.h"

using namespace lekmod_portable;

namespace {

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
		printf("saveload: %-4s %s: %s\n", ok ? "ok" : "FAIL", name, detail);
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
	std::vector<std::string> lines;
	Seen() : outer(NULL), logErrors(0), logWarnings(0), luaErrors(0) {}
};

void OnLog(void* user, int32_t level, const char* msg)
{
	Seen* s = static_cast<Seen*>(user);
	if (level == GC_LOG_ERROR) ++s->logErrors;
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
	if (ev->kind == GC_EVENT_LUA_ERROR)
	{
		++s->luaErrors;
		if (s->lines.size() < 20) s->lines.push_back(std::string("lua error: ") + (ev->text ? ev->text : ""));
	}
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

// The digests one phase saw: per viewer right after the save / load, and per turn (-1, human team) after it.
struct Digests
{
	int saveTurn;
	int humanTeam;
	std::map<int, std::string> view;               // viewer -> digest right after the save
	std::vector<std::pair<int, std::string> > turns;  // turn -> "digest-1 digestHuman"
	Digests() : saveTurn(-1), humanTeam(-1) {}

	std::string Text() const
	{
		std::string s = Format("save_turn %d\nhuman_team %d\n", saveTurn, humanTeam);
		for (std::map<int, std::string>::const_iterator it = view.begin(); it != view.end(); ++it) s += Format("view %d %s\n", it->first, it->second.c_str());
		for (size_t i = 0; i < turns.size(); ++i) s += Format("turn %d %s\n", turns[i].first, turns[i].second.c_str());
		return s;
	}
	bool Parse(const std::string& text)
	{
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos) end = text.size();
			const std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			char a[64], b[64];
			int n = 0;
			if (sscanf(line.c_str(), "save_turn %d", &n) == 1) saveTurn = n;
			else if (sscanf(line.c_str(), "human_team %d", &n) == 1) humanTeam = n;
			else if (sscanf(line.c_str(), "view %d %63s", &n, a) == 2) view[n] = a;
			else if (sscanf(line.c_str(), "turn %d %63s %63s", &n, a, b) == 3) turns.push_back(std::make_pair(n, std::string(a) + " " + b));
		}
		return saveTurn >= 0 && !view.empty();
	}
};

// gc_pump until the game turn reaches `target` (or a stop); per turn change the two digests. Returns the stop reason ("" = reached).
std::string PlayTurns(gc_instance* inst, int target, int humanTeam, const char* phase, Digests& d, int& stuck, long long& calls)
{
	gc_pump_result pr;
	memset(&pr, 0, sizeof(pr));
	gc_error err;
	int turn = GC.getGame().getGameTurn();
	const long long maxCalls = 20000LL * (target - turn + 1);
	long long mine = 0;
	while (turn < target)
	{
		if (++mine > maxCalls) return Format("turn %d: %lld gc_pump calls without reaching turn %d", turn, maxCalls, target);
		++calls;
		const int32_t rc = gc_pump(inst, 64, &pr, &err);
		if (pr.status == GC_PUMP_STUCK) { ++stuck; return Format("STUCK at turn %d: %s", pr.turn, err.message); }
		if (pr.status == GC_PUMP_GAME_OVER) return Format("GAME_OVER at turn %d", pr.turn);
		if (rc != GC_OK) return Format("gc_pump at turn %d: %d %s", pr.turn, rc, err.message);
		if (pr.turn != turn)
		{
			turn = pr.turn;
			const std::string a = Hex(Digest(inst, -1)), b = Hex(Digest(inst, humanTeam));
			printf("saveload-turn|%s|%d|%s|%s\n", phase, turn, a.c_str(), b.c_str());
			// Diagnostics for a divergence: sync RNG call count and the coarse state digest (RtSession::StateText) per turn.
			printf("saveload-state|%s|%d|rng %u|state %016llx\n", phase, turn, (unsigned)GC.getGame().getJonRand().getCallCount(), (unsigned long long)RtSession::StateDigest());
			d.turns.push_back(std::make_pair(turn, a + " " + b));
		}
	}
	return std::string();
}

std::string Num(long long v) { return Format("%lld", v); }

// Modding.OpenSaveData (B10 data packet, item 63): Lekmod's Lua keeps values in the DLL's Civ5SavedGameDatabase.db, which CvGame::Write embeds in
// the save and CvGame::Read writes back. `src` is Lua source returning one value; the result as text ("<error>" when the chunk fails).
std::string LuaText(RtSession* session, const char* src)
{
	RtScriptSystem* ss = session ? session->Scripts() : NULL;
	lua_State* L = ss ? ss->GetMasterState() : NULL;
	if (!L) return "<no lua>";
	const std::string chunk = std::string("B10_RESULT = tostring((function() ") + src + " end)()";
	const std::string wrapped = chunk + ")";
	if (!ss->RunString(L, wrapped.c_str(), "@b10_savedata.lua")) return "<error>";
	lua_getglobal(L, "B10_RESULT");
	const char* s = lua_tostring(L, -1);
	std::string out = s ? s : "<nil>";
	lua_pop(L, 1);
	return out;
}

const char* const kSaveDataSet =
	"local d = Modding.OpenSaveData() d.SetValue('b10_number', 4242) d.SetValue('b10_text', 'forty-two b10') return 'set'";
const char* const kSaveDataLate = "local d = Modding.OpenSaveData() d.SetValue('b10_late', 'after the turns') d.SetValue('b10_number', 4243) return 'set'";
const char* const kSaveDataGet =
	"local d = Modding.OpenSaveData() return tostring(d.GetValue('b10_number')) .. '|' .. tostring(d.GetValue('b10_text')) .. '|' .. tostring(d.GetValue('b10_late'))";
const char* const kSaveDataWant = "4243|forty-two b10|after the turns";

bool Contains(const std::vector<uint8_t>& bytes, const char* needle)
{
	const size_t n = strlen(needle);
	for (size_t i = 0; i + n <= bytes.size(); ++i)
		if (memcmp(&bytes[i], needle, n) == 0) return true;
	return false;
}

// A copy of a save with its body checksum recomputed (for the build-id and version tests, which must get past the checksum).
void Reseal(std::vector<uint8_t>& b)
{
	if (b.size() < kGcsFixedHeader) return;
	const uint64_t h = Xxh64(&b[kGcsFixedHeader], b.size() - kGcsFixedHeader, 0);
	for (int i = 0; i < 8; ++i) b[24 + i] = (uint8_t)(h >> (8 * i));
}

}  // namespace

extern "C" int rt_selftest_saveload(const gc_host* host, const char* phase, const char* save_file, unsigned seed, unsigned ai, unsigned save_at, unsigned cont,
	unsigned strict, char* report, size_t report_len)
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
	const double t0 = NowMs();
	std::string out;
	int stuck = 0, saveTurn = -1, endTurn = -1;
	long long calls = 0, saveBytes = 0;
	double msSave = 0, msLoad = 0, msTurns = 0;
	std::string stopReason, buildId;

	if ((!savePhase && !loadPhase) || file.empty())
	{
		c.add("args", false, "phase must be save or load and save_file is required (phase '%s')", phase ? phase : "");
	}
	gc_instance* inst = c.failed ? NULL : gc_create(&mine, &err);
	if (!c.failed) c.add("create", inst != NULL, "gc_create: %d %s", err.code, err.message);
	RtSession* session = InstanceSession(inst);
	if (session) buildId = session->BuildId();

	if (inst && savePhase)
	{
		const std::string setup = Format("{\"game_type\":\"single\",\"seed\":%u,\"ai_count\":%u,\"autoplay_human\":1}", seed, ai);
		int32_t rc = gc_new_game(inst, setup.c_str(), &err);
		c.add("new_game", rc == GC_OK, "%s: %d %s", setup.c_str(), rc, err.message);
		gc_buf buf = {0, 0, 0, 0};
		rc = gc_save(inst, &buf, &err);
		c.add("save_refused_before_pump", rc == GC_ERR_REJECTED, "gc_save before the first gc_pump: %d %s", rc, err.message);
		gc_buf_release(&buf);
		const PlayerTypes me = session->Engine().GetActivePlayer();
		const int humanTeam = me != NO_PLAYER ? (int)GET_PLAYER(me).getTeam() : -1;
		Digests d;
		d.humanTeam = humanTeam;
		const double tPlay = NowMs();
		// Modding.OpenSaveData values written at the start of the game (item 63).
		const std::string setR = LuaText(session, kSaveDataSet);
		c.add("savedata_set", setR == "set", "Modding.OpenSaveData().SetValue before the first turn: %s", setR.c_str());
		// To turn T, then pump on until gc_save accepts (the first save point at or after T).
		stopReason = PlayTurns(inst, (int)save_at, humanTeam, "save", d, stuck, calls);
		// Values written (and one overwritten) after the turns were played, just before the save.
		const std::string lateR = LuaText(session, kSaveDataLate);
		c.add("savedata_set_late", lateR == "set", "SetValue after turn %d: %s", GC.getGame().getGameTurn(), lateR.c_str());
		int refused = 0;
		std::string firstRefusal;
		std::vector<uint8_t> bytes;
		gc_pump_result pr;
		memset(&pr, 0, sizeof(pr));
		while (stopReason.empty())
		{
			gc_buf b = {0, 0, 0, 0};
			const double ts = NowMs();
			rc = gc_save(inst, &b, &err);
			if (rc == GC_OK)
			{
				msSave = NowMs() - ts;
				bytes.assign(b.data, b.data + b.len);
				gc_buf_release(&b);
				break;
			}
			if (rc != GC_ERR_REJECTED) { stopReason = Format("gc_save: %d %s", rc, err.message); break; }
			if (firstRefusal.empty()) firstRefusal = err.message;
			if (++refused > 200000) { stopReason = "no save point within 200000 gc_pump calls"; break; }
			++calls;
			const int32_t prc = gc_pump(inst, 64, &pr, &err);
			if (prc != GC_OK || pr.status == GC_PUMP_STUCK || pr.status == GC_PUMP_GAME_OVER) { stopReason = Format("gc_pump while looking for a save point: %d %s", prc, err.message); break; }
		}
		saveTurn = GC.getGame().getGameTurn();
		saveBytes = (long long)bytes.size();
		c.add("saved", stopReason.empty() && !bytes.empty(), "turn %d: %lld bytes in %.1f ms; %d gc_save refusal(s) before (first: %s)%s%s", saveTurn, saveBytes, msSave, refused,
			firstRefusal.c_str(), stopReason.empty() ? "" : "; ", stopReason.c_str());
		if (!bytes.empty())
		{
			d.saveTurn = saveTurn;
			// The values are inside the save (the embedded SQLite database holds the names and the text as plain bytes).
			c.add("savedata_in_save", Contains(bytes, "b10_number") && Contains(bytes, "forty-two b10") && Contains(bytes, "after the turns"), "the embedded database of the %lld-byte save holds the SimpleValues rows", saveBytes);
			const std::vector<int> viewers = Viewers();
			for (size_t i = 0; i < viewers.size(); ++i) d.view[viewers[i]] = Hex(Digest(inst, viewers[i]));
			// The save leaves the game alone and is reproducible: a second save is byte-identical, and so are the digests.
			gc_buf b2 = {0, 0, 0, 0};
			rc = gc_save(inst, &b2, &err);
			const bool same = rc == GC_OK && b2.len == bytes.size() && memcmp(b2.data, &bytes[0], bytes.size()) == 0;
			gc_buf_release(&b2);
			bool digestsSame = true;
			for (size_t i = 0; i < viewers.size(); ++i) digestsSame = digestsSame && d.view[viewers[i]] == Hex(Digest(inst, viewers[i]));
			c.add("save_stable", same && digestsSame, "second gc_save: %d, bytes identical %d, digests unchanged %d (%d viewer(s))", rc, same ? 1 : 0, digestsSame ? 1 : 0, (int)viewers.size());
			rc = gc_load_game(inst, &bytes[0], (uint32_t)bytes.size(), &err);
			c.add("load_on_running_instance", rc == GC_ERR_ALREADY_EXISTS, "gc_load_game on the instance that runs the game: %d %s", rc, err.message);
			c.add("write_save", WriteFile(file, &bytes[0], bytes.size()), "%s (%lld bytes)", file.c_str(), saveBytes);
			std::string vline;
			for (std::map<int, std::string>::const_iterator it = d.view.begin(); it != d.view.end(); ++it) vline += Format(" %d:%s", it->first, it->second.c_str());
			printf("saveload-view|save|%d|%s\n", saveTurn, vline.c_str());
			printf("saveload-state|save|%d|rng %u|state %016llx\n", saveTurn, (unsigned)GC.getGame().getJonRand().getCallCount(), (unsigned long long)RtSession::StateDigest());
			// N more turns with the same bot; the digest file lists only the turns after the save.
			d.turns.clear();
			stopReason = PlayTurns(inst, saveTurn + (int)cont, humanTeam, "save", d, stuck, calls);
			endTurn = GC.getGame().getGameTurn();
			c.add("continued", stopReason.empty(), "turn %d -> %d%s%s", saveTurn, endTurn, stopReason.empty() ? "" : ": ", stopReason.c_str());
			const std::string text = d.Text();
			c.add("write_digests", WriteFile(digestFile, text.data(), text.size()), "%s", digestFile.c_str());
		}
		msTurns = NowMs() - tPlay;
	}

	int firstDiverged = -1, turnsCompared = 0, viewsCompared = 0, viewsDiffer = 0;
	if (inst && loadPhase)
	{
		std::vector<uint8_t> bytes, dtext;
		Digests want;
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
				Bad x = {"garbage", std::vector<uint8_t>(bytes.begin(), bytes.begin() + 3), "GCS1"};
				bad.push_back(x);
			}
			{
				Bad x = {"not_gcs1", bytes, "GCS1"};
				x.b[0] = 'X';
				bad.push_back(x);
			}
			{
				Bad x = {"truncated", std::vector<uint8_t>(bytes.begin(), bytes.begin() + bytes.size() / 2), "truncated"};
				bad.push_back(x);
			}
			{
				Bad x = {"header_only", std::vector<uint8_t>(bytes.begin(), bytes.begin() + 20), "truncated"};
				bad.push_back(x);
			}
			{
				Bad x = {"flipped_byte", bytes, "checksum"};
				x.b[bytes.size() / 3] ^= 0x5a;
				bad.push_back(x);
			}
			{
				Bad x = {"format_version", bytes, "version"};
				x.b[4] = 99;
				bad.push_back(x);
			}
			{
				// Another build: change one character of the build id in the header section and reseal the checksum.
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
				const bool ok = rc == GC_ERR_BAD_DATA && strstr(err.message, bad[i].expect) != NULL;
				c.add((std::string("bad_") + bad[i].name).c_str(), ok, "%u bytes: %d %s", (unsigned)bad[i].b.size(), rc, err.message);
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
				saveTurn = GC.getGame().getGameTurn();
				std::string vline, diff;
				for (std::map<int, std::string>::const_iterator it = want.view.begin(); it != want.view.end(); ++it)
				{
					const std::string got = Hex(Digest(inst, it->first));
					++viewsCompared;
					vline += Format(" %d:%s", it->first, got.c_str());
					if (got != it->second) { ++viewsDiffer; diff += Format(" viewer %d: %s, saved %s;", it->first, got.c_str(), it->second.c_str()); }
				}
				printf("saveload-view|load|%d|%s\n", saveTurn, vline.c_str());
				printf("saveload-state|load|%d|rng %u|state %016llx\n", saveTurn, (unsigned)GC.getGame().getJonRand().getCallCount(), (unsigned long long)RtSession::StateDigest());
				const std::vector<int> viewers = Viewers();
				// Modding.OpenSaveData after the load: the values the save phase wrote (CvGame::Read wrote the embedded database back and the
				// Lua connection reopens it), and the object still takes writes.
				const std::string gotSd = LuaText(session, kSaveDataGet);
				c.add("savedata_after_load", gotSd == kSaveDataWant, "GetValue after the load: '%s' (want '%s')", gotSd.c_str(), kSaveDataWant);
				const std::string wrote = LuaText(session, "local d = Modding.OpenSaveData() d.SetValue('b10_after', 7) return tostring(d.GetValue('b10_after'))");
				c.add("savedata_write_after_load", wrote == "7", "SetValue after the load: '%s'", wrote.c_str());
				c.add("digests_after_load", saveTurn == want.saveTurn && viewsDiffer == 0 && viewers.size() == want.view.size(), "turn %d (saved at %d): %d/%d viewer digest(s) equal%s", saveTurn,
					want.saveTurn, viewsCompared - viewsDiffer, viewsCompared, diff.c_str());
				gc_buf b = {0, 0, 0, 0};
				const int32_t rs = gc_save(inst, &b, &err);
				const bool same = rs == GC_OK && b.len == bytes.size() && memcmp(b.data, &bytes[0], bytes.size()) == 0;
				const long long len = (long long)b.len;
				gc_buf_release(&b);
				// Informational: the DLL's own load path changes a few bytes (CvGame::Read counts a session, RecalculateLandmasses, ...).
				printf("saveload: info resave right after the load: rc %d, %lld bytes, byte-identical to the loaded save: %d\n", rs, len, same ? 1 : 0);
				int32_t again = gc_load_game(inst, &bytes[0], (uint32_t)bytes.size(), &err);
				c.add("second_load", again == GC_ERR_ALREADY_EXISTS, "a second gc_load_game: %d %s", again, err.message);
				again = gc_new_game(inst, "{\"game_type\":\"single\",\"seed\":1}", &err);
				c.add("new_game_after_load", again == GC_ERR_ALREADY_EXISTS, "gc_new_game after the load: %d %s", again, err.message);

				Digests got;
				const double tPlay = NowMs();
				stopReason = PlayTurns(inst, saveTurn + (int)cont, want.humanTeam, "load", got, stuck, calls);
				msTurns = NowMs() - tPlay;
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
				c.add("continued", stopReason.empty() && (int)got.turns.size() == (int)want.turns.size(), "turn %d -> %d, %d turn(s) (save phase %d)%s%s", saveTurn, endTurn, (int)got.turns.size(),
					(int)want.turns.size(), stopReason.empty() ? "" : ": ", stopReason.c_str());
				const bool sameTurns = firstDiverged < 0 && turnsCompared == (int)want.turns.size();
				const std::string detail = Format("%d turn(s) compared (digests of -1 and team %d), first difference %s", turnsCompared, want.humanTeam, first.empty() ? "none" : first.c_str());
				// Strict (run.sh --strict-continuation 1): the loaded game must play on exactly like the saved one. Otherwise reported only: the
				// golden DLL's own load path does not restore every AI input (civhost DS-0010 / DS-0019..0022, fixed only under LEKMOD_HEADLESS_FIX_*).
				if (strict) c.add("same_continuation", sameTurns, "%s", detail.c_str());
				else printf("saveload: %-4s same_continuation (not strict): %s\n", sameTurns ? "info" : "WARN", detail.c_str());
			}
		}
	}
	if (inst)
	{
		c.add("no_stuck", stuck == 0, "%d STUCK", stuck);
		c.add("lua_errors", seen.luaErrors == 0, "%d LUA_ERROR event(s)", seen.luaErrors);
		c.add("log_errors", seen.logErrors == 0, "%d error(s), %d warning(s) logged", seen.logErrors, seen.logWarnings);
	}
	for (size_t i = 0; i < seen.lines.size(); ++i) printf("saveload-event|%s\n", seen.lines[i].c_str());
	const RtAutoplay* bot = InstanceAutoplay(inst);
	const double msTotal = NowMs() - t0;
	out = "{\"failed\":" + Num(c.failed) + ",\"phase\":\"" + Escape(phase ? phase : "") + "\",\"seed\":" + Num(seed) + ",\"ai_count\":" + Num(ai) + ",\"save_at\":" + Num(save_at) +
		",\"continue\":" + Num(cont) + ",\"save_turn\":" + Num(saveTurn) + ",\"end_turn\":" + Num(endTurn) + ",\"save_bytes\":" + Num(saveBytes) + ",\"stuck\":" + Num(stuck) +
		",\"log_errors\":" + Num(seen.logErrors) + ",\"lua_errors\":" + Num(seen.luaErrors) + ",\"views_compared\":" + Num(viewsCompared) + ",\"views_differ\":" + Num(viewsDiffer) +
		",\"turns_compared\":" + Num(turnsCompared) + ",\"first_diverged\":" + Num(firstDiverged) + ",\"strict\":" + Num(strict) + ",\"pump_calls\":" + Num(calls) +
		",\"bot_commands\":" + Num(bot ? bot->Stats().commands : 0) + ",\"build_id\":\"" + Escape(buildId) + "\"" +
		",\"ms\":{\"save\":" + Format("%.1f", msSave) + ",\"load\":" + Format("%.1f", msLoad) + ",\"turns\":" + Format("%.1f", msTurns) + ",\"total\":" + Format("%.1f", msTotal) + "}" +
		",\"stop_reason\":\"" + Escape(stopReason) + "\",\"checks\":[" + c.json + "]}";
	if (report && report_len)
	{
		strncpy(report, out.c_str(), report_len - 1);
		report[report_len - 1] = '\0';
	}
	printf("saveload-summary|%s|seed %u|ai %u|save turn %d|end turn %d|bytes %lld|save %.0f ms|load %.0f ms|views %d/%d|turns compared %d|first diverged %d|failed %d\n", phase ? phase : "?", seed, ai,
		saveTurn, endTurn, saveBytes, msSave, msLoad, viewsCompared - viewsDiffer, viewsCompared, turnsCompared, firstDiverged, c.failed);
	gc_destroy(inst);
	return c.failed == 0 ? 0 : 1;
}
