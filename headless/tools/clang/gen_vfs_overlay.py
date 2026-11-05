#!/usr/bin/env python3
"""Writes a clang VFS overlay that makes header lookup case-insensitive (MSVC sources include
"Fireworks/FDataStream.h" for FireWorks/..., "ICvDllUserInterface.h" for ICvDLLUserInterface.h, ...).

Usage: gen_vfs_overlay.py OUT.yaml DIR [DIR...]
Every header-like file under each DIR is mapped onto itself; lookups through -I paths then match
any letter case. Sources stay untouched."""
import json
import os
import sys

EXTS = ('.h', '.hpp', '.inl', '')


def tree(path):
    contents = []
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if os.path.isdir(full):
            sub = tree(full)
            if sub:
                contents.append({'type': 'directory', 'name': name, 'contents': sub})
        elif os.path.splitext(name)[1].lower() in EXTS:
            contents.append({'type': 'file', 'name': name, 'external-contents': full})
    return contents


def main():
    out, dirs = sys.argv[1], sys.argv[2:]
    roots = []
    for d in dirs:
        d = os.path.realpath(d)
        roots.append({'type': 'directory', 'name': d, 'contents': tree(d)})
    doc = {'version': 0, 'case-sensitive': 'false', 'roots': roots}
    tmp = out + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(doc, f, indent=1)
    os.replace(tmp, out)


if __name__ == '__main__':
    main()
