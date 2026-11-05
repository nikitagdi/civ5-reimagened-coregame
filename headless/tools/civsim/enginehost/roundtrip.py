"""Save/load round-trip check: which serialized bytes change when a checkpoint is loaded and written again.

usage:
  python -m civsim.enginehost.roundtrip <turn-NNNN.ckpt> [--dll <dll>] [--out <run dir>] [--label after-read|after-postload|after-start]
  python -m civsim.enginehost.roundtrip --compare <turn-NNNN.ckpt> <run dir>     (no host run; reuse a --ckpt-verify run)

Runs `civhost --load <ckpt> --ckpt-verify` in a fresh process (no turn is played). The host re-serializes the state
after the DLL Read calls (after-read), after the exe's post-load tail (after-postload) and after the contexts/L8/L9
(after-start), and dumps every segment that differs from the checkpoint to <run>\\ckpt-verify-<label>\\<segment>.bin.
This tool prints, per label and segment, the size change and the differing byte ranges (same size: every range;
different size: common prefix / common suffix bounds of the changed region).
Exit code: 0 all labels clean, 1 differences, 2 bad input / host failure.
"""
import os
import struct
import subprocess
import sys

LABELS = ('after-read', 'after-postload', 'after-start')
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..'))
EXE = os.path.join(REPO, 'headless', 'out', 'host', 'Release', 'civhost.exe')


def read_checkpoint(path):
    """CHCK v1 (headless/host/src/game/HstCheckpoint.h) -> {section name: bytes}."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'CHCK':
        raise ValueError('%s: bad magic' % path)
    version, count = struct.unpack_from('<II', data, 4)
    if version != 1:
        raise ValueError('%s: unsupported version %d' % (path, version))
    pos = 12
    sections = {}
    for _ in range(count):
        (nlen,) = struct.unpack_from('<H', data, pos)
        pos += 2
        name = data[pos:pos + nlen].decode('utf-8')
        pos += nlen
        size, _hash = struct.unpack_from('<IQ', data, pos)
        pos += 12
        sections[name] = data[pos:pos + size]
        pos += size
    return sections


def diff_ranges(a, b, gap=8, limit=64):
    """Differing [start, end) ranges of two equal-length byte strings, ranges closer than `gap` merged."""
    out = []
    i, n = 0, len(a)
    while i < n and len(out) < limit:
        if a[i] == b[i]:
            i += 1
            continue
        start = i
        last = i
        while i < n and i - last <= gap:
            if a[i] != b[i]:
                last = i
            i += 1
        out.append((start, last + 1))
    return out


def describe(name, old, new):
    lines = []
    if len(old) == len(new):
        rs = diff_ranges(old, new)
        lines.append('  %-10s %d bytes, %d range(s) differ' % (name, len(old), len(rs)))
        for s, e in rs[:24]:
            lines.append('    [%7d, %7d) old %s new %s' % (s, e, old[s:min(e, s + 16)].hex(), new[s:min(e, s + 16)].hex()))
        if len(rs) > 24:
            lines.append('    ... %d more' % (len(rs) - 24))
    else:
        p = 0
        m = min(len(old), len(new))
        while p < m and old[p] == new[p]:
            p += 1
        s = 0
        while s < m - p and old[len(old) - 1 - s] == new[len(new) - 1 - s]:
            s += 1
        lines.append('  %-10s %d -> %d bytes (%+d): common prefix %d, common suffix %d; changed old[%d:%d] -> new[%d:%d]'
                     % (name, len(old), len(new), len(new) - len(old), p, s, p, len(old) - s, p, len(new) - s))
        lines.append('    old@%d %s' % (p, old[p:p + 24].hex()))
        lines.append('    new@%d %s' % (p, new[p:p + 24].hex()))
    return lines


def compare(ckpt, run_dir, labels=LABELS):
    sections = read_checkpoint(ckpt)
    dirty = False
    for label in labels:
        d = os.path.join(run_dir, 'ckpt-verify-' + label)
        if not os.path.isdir(d):
            print('%s: no dump folder (%s)' % (label, d))
            continue
        files = sorted(f for f in os.listdir(d) if f.endswith('.bin'))
        print('%s: %d segment(s) differ' % (label, len(files)))
        groups = {}	# player/team segments with the same size change and old/new bytes at the change: print once
        for f in files:
            name = f[:-4]
            with open(os.path.join(d, f), 'rb') as fh:
                new = fh.read()
            old = sections.get(name, b'')
            lines = describe(name, old, new)
            if name[:4] in ('play', 'team') and len(old) != len(new):
                key = (name[:4], len(new) - len(old), lines[1][lines[1].index(' ', 6):], lines[2][lines[2].index(' ', 6):])
                if key in groups:
                    groups[key].append(name)
                    continue
                groups[key] = [name]
            for line in lines:
                print(line)
        for key, names in groups.items():
            if len(names) > 1:
                print('  (same size change and bytes as %s: %s)' % (names[0], ' '.join(names[1:])))
        dirty = dirty or bool(files)
    return dirty


def main(argv):
    args = list(argv)
    if not args or args[0] in ('-h', '--help'):
        print(__doc__)
        return 2
    labels = LABELS
    if '--label' in args:
        i = args.index('--label')
        labels = (args[i + 1],)
        del args[i:i + 2]
    if args[0] == '--compare':
        if len(args) != 3:
            print(__doc__)
            return 2
        return 1 if compare(args[1], args[2], labels) else 0
    ckpt = os.path.abspath(args.pop(0))
    dll = None
    out = None
    while args:
        k = args.pop(0)
        if k == '--dll':
            dll = os.path.abspath(args.pop(0))
        elif k == '--out':
            out = os.path.abspath(args.pop(0))
        else:
            print(__doc__)
            return 2
    if out is None:
        out = os.path.join(os.path.dirname(os.path.dirname(ckpt)), 'roundtrip-' + os.path.splitext(os.path.basename(ckpt))[0])
    # The host only adds dumps of differing segments: drop dumps a previous run left in the same folder.
    for label in LABELS:
        d = os.path.join(out, 'ckpt-verify-' + label)
        if os.path.isdir(d):
            for f in os.listdir(d):
                os.remove(os.path.join(d, f))
    cmd = [EXE, '--load', ckpt, '--ckpt-verify', '--out', out]
    if dll:
        cmd += ['--dll', dll]
    r = subprocess.run(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        print('civhost exit %d: %s' % (r.returncode, r.stdout.decode('utf-8', 'replace')[-2000:]))
        return 2
    print('run dir %s' % out)
    return 1 if compare(ckpt, out, labels) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
