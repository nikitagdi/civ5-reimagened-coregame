"""Compare two civhost digest.bin files (format v1, see headless/host/src/diag/HstDigest.h).

usage: python -m civsim.enginehost.digest_diff A/digest.bin B/digest.bin [--all]
Compares the turns present in both files (a run resumed with --load starts later); prints the first turn whose hashes differ and the differing segments (all differing turns with --all).
Exit code: 0 identical, 1 different, 2 bad input.
"""
import struct
import sys


def read_digest(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'CSDG':
        raise ValueError('%s: bad magic' % path)
    version, flags, count = struct.unpack_from('<HHI', data, 4)
    if version != 1:
        raise ValueError('%s: unsupported version %d' % (path, version))
    pos = 12
    names = []
    for _ in range(count):
        (length,) = struct.unpack_from('<H', data, pos)
        pos += 2
        names.append(data[pos:pos + length].decode('utf-8'))
        pos += length
    record = 4 + 8 * count
    turns = []
    while pos + record <= len(data):
        (turn,) = struct.unpack_from('<I', data, pos)
        hashes = struct.unpack_from('<%dQ' % count, data, pos + 4)
        turns.append((turn, hashes))
        pos += record
    return names, turns


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    show_all = '--all' in argv
    if len(args) != 2:
        print(__doc__)
        return 2
    try:
        names_a, turns_a = read_digest(args[0])
        names_b, turns_b = read_digest(args[1])
    except (IOError, ValueError) as e:
        print(e)
        return 2
    if names_a != names_b:
        print('segment lists differ')
        return 1
    print('%d segments; %d vs %d turn records' % (len(names_a), len(turns_a), len(turns_b)))
    # Align by turn number: a run resumed from a checkpoint (--load) starts later than the continuous run.
    by_turn_b = dict(turns_b)
    common = [(ta, ha, by_turn_b[ta]) for ta, ha in turns_a if ta in by_turn_b]
    if len(common) != len(turns_a) or len(common) != len(turns_b):
        if common:
            print('comparing %d common turns (%d..%d)' % (len(common), common[0][0], common[-1][0]))
        else:
            print('no common turns')
            return 1
    different = False
    for ta, ha, hb in common:
        diff = [names_a[i] for i in range(len(names_a)) if ha[i] != hb[i]]
        if diff:
            if not different:
                print('FIRST DIFFERENCE at turn %d: %s' % (ta, ' '.join(diff)))
            elif show_all:
                print('turn %d: %s' % (ta, ' '.join(diff)))
            different = True
            if not show_all:
                break
    if not different:
        print('identical')
    return 1 if different else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))