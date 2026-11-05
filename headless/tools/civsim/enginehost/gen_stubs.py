"""Generate civhost engine-interface stubs from the SDK interface headers.

For every engine interface family the host implements, emits a class ``Hst<Family>Gen`` that overrides every pure
virtual of the interface chain with the exact signature (calling convention, const, overload set).  Each body counts
the call and dispatches on the class from ``headless/host/api/engine-api.csv``:

  sink        no-op, returns a zero value
  query       returns the CSV constant (``value`` column)
  sim         HstUnimplemented(id) -> abort; the hand-written subclass must override it
  gen         generated real behaviour (ICvUnknown QueryInterface / Destroy)
  (missing)   HstUnexpected(id) -> logged, abort under --strict

Usage:
  python -m civsim.enginehost.gen_stubs            # regenerate headless/host/src/engine/gen/*
  python -m civsim.enginehost.gen_stubs --bootstrap-csv   # (re)create the CSV from built-in rules, keeping rows
  python -m civsim.enginehost.gen_stubs --list     # print parsed methods
Run from headless/tools (or pass --repo).  Output is committed so vcbuild needs no Python.
"""
import argparse
import csv
import os
import re
import sys

SDK_INC = os.path.join('LEKMOD_DLL', 'CvGameCoreDLL_Expansion2', 'CvGameCoreDLLUtil', 'include')

# family -> (header files, most-derived interface, chain root-first, has ICvUnknown)
FAMILIES = [
	('EngineUtility', ['CvDllInterfaces.h', 'ICvDLLUtility.h'], 'ICvEngineUtility4',
	 ['ICvUnknown', 'ICvEngineUtility1', 'ICvEngineUtility2', 'ICvEngineUtility3', 'ICvEngineUtility4']),
	('UserInterface', ['CvDllInterfaces.h', 'ICvDLLUserInterface.h'], 'ICvUserInterface2',
	 ['ICvUnknown', 'ICvUserInterface1', 'ICvUserInterface2']),
	('ScriptSystem', ['ICvDLLScriptSystem.h'], 'ICvEngineScriptSystem1',
	 ['ICvEngineScriptSystem1']),
]

SAL_RE = re.compile(r'\b_(?:In|Out|Inout|Ret|Deref)[A-Za-z0-9_]*_(?:\s*\([^()]*\))?')
STRIP_TOKENS = ['__checkReturn', 'DECLSPEC_DEPRECATED']


def strip_comments(s):
	s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
	s = re.sub(r'//[^\n]*', ' ', s)
	return s


def clean(s):
	s = SAL_RE.sub(' ', s)
	for t in STRIP_TOKENS:
		s = re.sub(r'\b%s\b' % t, ' ', s)
	return ' '.join(s.split())


def split_top(s, sep=','):
	out, depth, cur = [], 0, ''
	for ch in s:
		if ch in '<([':
			depth += 1
		elif ch in '>)]':
			depth -= 1
		if ch == sep and depth == 0:
			out.append(cur)
			cur = ''
		else:
			cur += ch
	if cur.strip():
		out.append(cur)
	return out


def class_bodies(text):
	"""Yield (name, base, body) for 'class X [: public Y] { ... };'."""
	for m in re.finditer(r'\bclass\s+(\w+)\s*(?::\s*public\s+(\w+))?\s*\{', text):
		i = m.end()
		depth = 1
		while depth and i < len(text):
			if text[i] == '{':
				depth += 1
			elif text[i] == '}':
				depth -= 1
			i += 1
		yield m.group(1), m.group(2), text[m.end():i - 1]


class Method(object):
	def __init__(self, iface, ret, name, params, const, stdcall):
		self.iface, self.ret, self.name, self.params, self.const, self.stdcall = iface, ret, name, params, const, stdcall
		self.id = None  # filled later

	def sig(self, qual=''):
		cc = '__stdcall ' if self.stdcall else ''
		ps = ', '.join('%s %s%s' % (t, n, suf) for (t, n, suf) in self.params)
		return '%s %s%s%s(%s)%s' % (self.ret, cc, qual, self.name, ps, ' const' if self.const else '')

	def param_types(self):
		return ', '.join(t + suf for (t, n, suf) in self.params)


def parse_param(p, idx):
	p = clean(p.split('=')[0]) if '=' in p and '==' not in p else clean(p)
	if p in ('', 'void'):
		return None
	suffix = ''
	m = re.match(r'^(.*?)(\[\s*\])$', p)
	if m:
		p, suffix = m.group(1).strip(), '[]'
	if p.endswith('*') or p.endswith('&') or p.endswith('>'):
		return (p, 'p%d' % idx, suffix)
	m = re.match(r'^(.*[\s\*&])(\w+)$', p)
	words = [w for w in re.findall(r'\w+', p) if w not in ('const', 'unsigned', 'signed', 'volatile')]
	if m and len(words) >= 2:
		return (m.group(1).strip(), m.group(2), suffix)
	return (p, 'p%d' % idx, suffix)


def parse_methods(iface, body):
	out = []
	for m in re.finditer(r'\bvirtual\b(.*?)=\s*0\s*;', body, flags=re.S):
		decl = clean(m.group(1))
		if decl.startswith('~'):
			continue
		lp = decl.index('(')
		head = decl[:lp].strip()
		# matching paren
		depth, j = 0, lp
		while True:
			if decl[j] == '(':
				depth += 1
			elif decl[j] == ')':
				depth -= 1
				if depth == 0:
					break
			j += 1
		params_s = decl[lp + 1:j]
		tail = decl[j + 1:].strip()
		const = tail == 'const'
		hm = re.match(r'^(.*?)\s*\b(\w+)$', head)
		ret_cc, name = hm.group(1).strip(), hm.group(2)
		stdcall = bool(re.search(r'\bDLLCALL\b', ret_cc))
		ret = ' '.join(re.sub(r'\bDLLCALL\b', ' ', ret_cc).split())
		params = []
		for k, p in enumerate(split_top(params_s)):
			pp = parse_param(p, k)
			if pp:
				params.append(pp)
		out.append(Method(iface, ret, name, params, const, stdcall))
	return out


def load_family(repo, fam):
	name, headers, derived, chain = fam
	text = ''
	for h in headers:
		with open(os.path.join(repo, SDK_INC, h), 'r') as f:
			text += strip_comments(f.read()) + '\n'
	classes = {}
	for cname, base, body in class_bodies(text):
		if cname in chain and cname not in classes:
			classes[cname] = (base, body)
	missing = [c for c in chain if c not in classes]
	if missing:
		raise SystemExit('missing interface classes %s for %s' % (missing, name))
	methods = []
	for c in chain:
		methods += parse_methods(c, classes[c][1])
	# ids: Family_Name, overloads get _1.._n in declaration order
	counts = {}
	for m in methods:
		counts[m.name] = counts.get(m.name, 0) + 1
	seen = {}
	for m in methods:
		if counts[m.name] > 1:
			seen[m.name] = seen.get(m.name, 0) + 1
			m.id = 'kHstApi_%s_%s_%d' % (name, m.name, seen[m.name])
		else:
			m.id = 'kHstApi_%s_%s' % (name, m.name)
	return methods


# ---------------------------------------------------------------------------------------------------------------------
# CSV bootstrap rules (design section 2.2).  The CSV is the source of truth once created; rerunning --bootstrap-csv
# only adds rows for methods that are missing.
SIM_ENGINE = set('''GetScriptSystem CanAdvanceTurn DoTurn SendAICivsProcessed allAICivsProcessedThisTurn allAIProcessedThisTurn
HasReceivedTurnComplete HasReceivedTurnAllComplete HasReceivedTurnAllCompleteFromAllPlayers HasSentTurnComplete
HasSentTurnAllComplete sendTurnComplete sendTurnUnready sendPlayerInitialAIProcessed HasReceivedAICivsProcessed
GetNumAICivsProcessed IsPlayerConnected GetCacheFolderPath GetEvaluatedMapScriptPath AutoSave SaveGame DoMapSetup
InitGlobals UninitGlobals getSecsPerTurn getTurnsPerMinute getMillisecsPerTurn getTurnsPerSecond'''.split())
SEND_PREFIXES = ('send', 'Send')
SEND_SINKS = set('''sendPlayerInfo sendGameInfo sendChat sendPing sendPause sendTurnReminder sendPbemTurn
sendUnitSyncCheck sendPlotSyncCheck sendCitySyncCheck sendRandomNumberGeneratorSyncCheck sendPlayerSyncCheck
SendGameDoTurnProcessed sendPlayerOption sendExtendedGame'''.split())
QUERY_ENGINE = {
	'getInterfaceIFace': 'NULL', 'IsHost': 'true', 'GetModsDatabase': 'NULL', 'IsModActivated': 'false',
	'IsDLCValid': 'true', 'getAssignedNetworkID': '-1', 'IsPitbossHost': 'false', 'GetAutorun': 'true',
	'GetDone': 'false', 'altKey': 'false', 'shiftKey': 'false', 'ctrlKey': 'false', 'getPlayerOption': 'false',
	'IsAchievementUnlocked': 'false', 'GetSteamStat': 'false', 'GetAdvisorBadAttackInterrupt': 'false',
	'GetAdvisorCityAttackInterrupt': 'false', 'GetTutorialLevel': '0', 'IsNetPlayer': 'false',
	'TunerConnected': 'false', 'TunerEverConnected': 'false', 'IsProcessingGameCoreMessages': 'false',
	'GetGameCoreLock': '', 'ReleaseGameCoreLock': '', 'TryGameCoreLock': 'true', 'HasGameCoreLock': 'false',
	'IsGameCoreThread': 'true', 'IsGameCoreExecuting': 'false', 'IsPlayerKicked': 'false',
	'IsPlayerHotJoining': 'false', 'ShouldValidateGameDatabase': 'false', 'getTransferredMap': 'false',
	'isWBMapScript': 'false', 'isWBMapNoPlayers': 'false', 'GetAudioTagIndex': '-1',
	'ReseatConnectedPlayer': 'false', 'endTurnTimerLength': '0.0f',
}
QUERY_UI = {
	'isDiploActive': 'false', 'isCityScreenUp': 'false', 'IsPopupQueueEmpty': 'true', 'canEndTurn': 'true',
	'IsSPAutoEndTurnEnabled': 'true', 'IsMPAutoEndTurnEnabled': 'true', 'IsOptionNoRewardPopups': 'true',
	'IsLoadedGame': 'false', 'GetHeadSelectedUnit': 'NULL', 'getSelectionPlot': 'NULL', 'isPopupUp': 'false',
	'isPopupQueued': 'false', 'isDiploOrPopupWaiting': 'false', 'IsModalStackEmpty': 'true',
	'IsDontShowPopups': 'true', 'IsSPQuickCombatEnabled': 'true', 'IsMPQuickCombatEnabled': 'true',
	'isInAdvancedStart': 'false', 'isFocused': 'false', 'isFocusedWidget': 'false', 'waitingForRemotePlayers': 'false',
	'IsDealInTransit': 'false', 'GetScratchDeal': 'NULL', 'IsPolicyNotificationSeen': 'true',
}


def bootstrap_class(family, m):
	if m.iface == 'ICvUnknown':
		return 'gen', ''
	if family == 'ScriptSystem':
		return 'sim', ''
	if family == 'EngineUtility':
		if m.name in SIM_ENGINE:
			return 'sim', ''
		if m.name in QUERY_ENGINE:
			return 'query', QUERY_ENGINE[m.name]
		if m.name.startswith(SEND_PREFIXES) and m.name not in SEND_SINKS:
			return 'sim', ''
		return 'sink', ''
	if family == 'UserInterface':
		if m.name in QUERY_UI:
			return 'query', QUERY_UI[m.name]
		return 'sink', ''
	return 'sink', ''


CSV_FIELDS = ['family', 'method', 'class', 'value', 'exe_rva', 'note']


def read_csv(path):
	rows = {}
	if os.path.exists(path):
		with open(path, 'r', newline='') as f:
			for r in csv.DictReader(f):
				rows[(r['family'], r['method'])] = r
	return rows


def write_csv(path, rows, order):
	with open(path, 'w', newline='') as f:
		w = csv.DictWriter(f, fieldnames=CSV_FIELDS, lineterminator='\n')
		w.writeheader()
		for k in order:
			w.writerow(rows[k])


# ---------------------------------------------------------------------------------------------------------------------
HDR = '// GENERATED by headless/tools/civsim/enginehost/gen_stubs.py from the SDK interface headers and\n' \
      '// headless/host/api/engine-api.csv. Do not edit; rerun: pwsh headless\\tools\\build-host.ps1 -Regen\n'


def body(m, cls, value):
	r = m.ret
	lines = ['\tHST_API(%s);' % m.id]
	zero = '' if r == 'void' else '\treturn HstZero< %s >();' % r
	if cls == 'gen':
		if m.name == 'QueryInterface':
			return None  # emitted specially
		if m.name == 'Destroy':
			return lines  # host owns the object; ICvUnknown::operator delete must not free it
	if cls == 'query':
		if r != 'void':
			lines.append('\treturn (%s)(%s);' % (r, value if value else '0'))
		return lines
	if cls == 'sim':
		lines.append('\tHstUnimplemented(%s);' % m.id)
	elif cls == 'sink':
		pass
	else:
		lines.append('\tHstUnexpected(%s);' % m.id)
	if zero:
		lines.append(zero)
	return lines


def generate(repo, families, rows, outdir):
	ids = []
	infos = []
	files = {}
	for (fam, methods), famdef in zip(families, FAMILIES):
		name, headers, derived, chain = famdef
		cname = 'Hst%sGen' % derived[3:]
		h = [HDR, '#pragma once', '', '#include "HstApi.h"', '',
		     'class %s : public %s' % (cname, derived), '{', 'public:']
		cpp = [HDR, '#include "HstCommon.h"', '#include "%s.h"' % cname, '']
		for m in methods:
			r = rows.get((name, m.name))
			cls, value = (r['class'], r['value']) if r else ('unexpected', '')
			ids.append(m.id)
			infos.append((name, m.name, m.iface, cls))
			h.append('\tvirtual %s;%s' % (m.sig(), '' if cls != 'sim' else '\t// sim'))
			if m.name == 'QueryInterface' and m.iface == 'ICvUnknown':
				guids = ['guid%s' % c for c in chain]
				cpp.append('%s\n{' % m.sig(cname + '::'))
				cpp.append('\tHST_API(%s);' % m.id)
				cpp.append('\tif(%s)' % ' ||\n\t\t'.join('%s == %s' % (m.params[0][1], g) for g in guids))
				cpp.append('\t\treturn this;')
				cpp.append('\treturn NULL;')
				cpp.append('}')
				continue
			b = body(m, cls, value)
			cpp.append('%s\n{\n%s\n}' % (m.sig(cname + '::'), '\n'.join(b)))
		h.append('};')
		files[cname + '.h'] = '\n'.join(h) + '\n'
		files[cname + '.cpp'] = '\n'.join(cpp) + '\n'
	# id enum + table
	e = [HDR, '#pragma once', '', 'enum HstApiId', '{']
	e += ['\t%s,' % i for i in ids]
	e += ['\tNUM_HST_API', '};', '']
	files['HstApiIds.h'] = '\n'.join(e) + '\n'
	t = [HDR, '#include "HstCommon.h"', '#include "HstApi.h"', '',
	     'unsigned int g_auiHstApiCalls[NUM_HST_API];', '',
	     'const HstApiInfo g_akHstApiInfo[NUM_HST_API] =', '{']
	for (fam, meth, iface, cls) in infos:
		t.append('\t{"%s", "%s", "%s", "%s"},' % (fam, meth, iface, cls))
	t += ['};', '']
	files['HstApiTable.cpp'] = '\n'.join(t) + '\n'
	os.makedirs(outdir, exist_ok=True)
	changed = 0
	for fn, content in sorted(files.items()):
		p = os.path.join(outdir, fn)
		old = open(p, 'r').read() if os.path.exists(p) else None
		if old != content:
			with open(p, 'w', newline='\r\n') as f:
				f.write(content)
			changed += 1
	return len(ids), changed


def main(argv=None):
	ap = argparse.ArgumentParser()
	here = os.path.dirname(os.path.abspath(__file__))
	ap.add_argument('--repo', default=os.path.normpath(os.path.join(here, '..', '..', '..', '..')))
	ap.add_argument('--bootstrap-csv', action='store_true')
	ap.add_argument('--list', action='store_true')
	a = ap.parse_args(argv)
	csv_path = os.path.join(a.repo, 'headless', 'host', 'api', 'engine-api.csv')
	outdir = os.path.join(a.repo, 'headless', 'host', 'src', 'engine', 'gen')
	families = [(f[0], load_family(a.repo, f)) for f in FAMILIES]
	if a.list:
		for fam, ms in families:
			for m in ms:
				print('%-14s %-22s %s' % (fam, m.iface, m.sig()))
		return 0
	rows = read_csv(csv_path)
	if a.bootstrap_csv:
		order = []
		for fam, ms in families:
			for m in ms:
				k = (fam, m.name)
				if k in order:
					continue
				order.append(k)
				if k not in rows:
					cls, val = bootstrap_class(fam, m)
					rows[k] = {'family': fam, 'method': m.name, 'class': cls, 'value': val, 'exe_rva': '', 'note': ''}
		os.makedirs(os.path.dirname(csv_path), exist_ok=True)
		write_csv(csv_path, rows, order)
		print('csv: %d rows -> %s' % (len(order), csv_path))
	n, changed = generate(a.repo, families, rows, outdir)
	unclassified = [(f, m.name) for f, ms in families for m in ms if (f, m.name) not in rows]
	print('gen: %d methods, %d files changed, %d unclassified -> %s' % (n, changed, len(unclassified), outdir))
	for u in unclassified:
		print('  unclassified: %s::%s' % u)
	return 0


if __name__ == '__main__':
	sys.exit(main())
