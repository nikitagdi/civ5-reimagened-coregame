"""civsim.baseline — PE section comparison for reproducibility / golden-neutrality checks.

Usage:
  python -m civsim.baseline compare <a.dll> <b.dll>   # section hashes; exit 0 if .text/.rdata/.data/.rsrc equal
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

CHECKED = (".text", ".rdata", ".data", ".rsrc")


def pe_sections(path: Path) -> dict[str, bytes]:
    data = bytearray(path.read_bytes())
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE file")
    n_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt = e_lfanew + 24
    sec_off = opt + opt_size
    secs = []
    for i in range(n_sections):
        off = sec_off + 40 * i
        name = data[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, off + 8)
        secs.append((name, vsize, va, rsize, rptr))

    def rva2off(rva: int) -> int | None:
        for _n, vs, va, rs, rp in secs:
            if va <= rva < va + max(vs, rs):
                return rp + rva - va
        return None

    # Normalize link timestamps: export directory (+4) and every debug directory entry (+4).
    exp_rva = struct.unpack_from("<I", data, opt + 96)[0]
    dbg_rva, dbg_size = struct.unpack_from("<II", data, opt + 96 + 6 * 8)
    if exp_rva and (o := rva2off(exp_rva)) is not None:
        data[o + 4:o + 8] = bytes(4)
    if dbg_rva and (o := rva2off(dbg_rva)) is not None:
        for k in range(dbg_size // 28):
            data[o + 28 * k + 4:o + 28 * k + 8] = bytes(4)
            # SizeOfData of the CodeView entry = 24 + len(PDB path) + 1: it changes with the output dir name length
            # (golden vs gcheck0); the path itself is blanked in strip_debug_dir.
            data[o + 28 * k + 16:o + 28 * k + 20] = bytes(4)
    return {n: bytes(data[rp:rp + (min(vs, rs) if vs else rs)]) for n, vs, _va, rs, rp in secs}


def strip_debug_dir(name: str, blob: bytes) -> bytes:
    # The debug directory (timestamp + PDB GUID/age) lives in .rdata; zero RSDS records so reproducible builds compare equal.
    if name != ".rdata":
        return blob
    b = bytearray(blob)
    i = b.find(b"RSDS")
    while i != -1:
        b[i:i + 24] = bytes(24)
        # also blank the NUL-terminated PDB path that follows (it contains the output dir name, e.g. ...\dll\golden\...)
        j = i + 24
        while j < len(b) and b[j] != 0 and j - i < 24 + 520:
            b[j] = 0
            j += 1
        i = b.find(b"RSDS", i + 24)
    return bytes(b)


def compare(a: Path, b: Path) -> int:
    sa, sb = pe_sections(a), pe_sections(b)
    rc = 0
    for name in sorted(set(sa) | set(sb)):
        ha = hashlib.sha256(strip_debug_dir(name, sa.get(name, b""))).hexdigest()[:16] if name in sa else "-"
        hb = hashlib.sha256(strip_debug_dir(name, sb.get(name, b""))).hexdigest()[:16] if name in sb else "-"
        same = ha == hb
        if name in CHECKED and not same:
            rc = 6
        print(f"{name:8} {ha} {hb} {'=' if same else 'DIFF'}")
    return rc


def obj_comdats(path: Path) -> dict[tuple[bytes, bytes], list[bytes]]:
    """COMDAT name -> raw bytes of each .text/.rdata/.data section of a COFF object (order-insensitive)."""
    d = path.read_bytes()
    _m, nsec, _ts, symptr, nsym, optsz, _ch = struct.unpack_from("<HHIIIHH", d, 0)
    off, secs = 20 + optsz, []
    for _ in range(nsec):
        name = d[off:off + 8].rstrip(b"\0")
        size, ptr = struct.unpack_from("<II", d, off + 16)
        secs.append((name, d[ptr:ptr + size] if ptr else b""))
        off += 40
    strtab, names, i = symptr + 18 * nsym, {}, 0
    while i < nsym:
        o = symptr + 18 * i
        raw = d[o:o + 8]
        if raw[:4] == b"\0\0\0\0":
            so = struct.unpack_from("<I", raw, 4)[0]
            nm = d[strtab + so:d.index(b"\0", strtab + so)]
        else:
            nm = raw.rstrip(b"\0")
        _val, secno, _typ, cls, naux = struct.unpack_from("<IhHBB", d, o + 8)
        if secno > 0 and cls == 2 and secno - 1 not in names:
            names[secno - 1] = nm
        i += 1 + naux
    out: dict[tuple[bytes, bytes], list[bytes]] = {}
    for idx, (sn, b) in enumerate(secs):
        if sn.startswith((b".text", b".rdata", b".data")):
            out.setdefault((sn, names.get(idx, b"?%d" % idx)), []).append(b)
    return out


def objcmp(a: Path, b: Path) -> int:
    """Function-level comparison of two object dirs (e.g. headless/out/obj/golden vs .../obj/<check>).
    Flag-guarded header edits reorder COMDATs and shift __LINE__ constants (allocation/debug macros) without changing
    behaviour; this reports only functions whose bytes differ, so those cases can be reviewed one by one."""
    rc = 0
    for f in sorted(p.name for p in a.glob("*.obj")):
        if not (b / f).exists():
            print(f"{f}: missing in {b}")
            rc = 6
            continue
        A, B = obj_comdats(a / f), obj_comdats(b / f)
        if A == B:
            continue
        changed = [k for k in set(A) & set(B) if sorted(A[k]) != sorted(B[k])]
        only = set(A) ^ set(B)
        if changed or only:
            rc = 6
            print(f"{f}: {len(changed)} changed, {len(only)} added/removed")
            for k in changed[:10]:
                print(f"    {k[1].decode('ascii', 'replace')[:100]}")
    return rc


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="civsim.baseline")
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare")
    c.add_argument("a", type=Path)
    c.add_argument("b", type=Path)
    o = sub.add_parser("objcmp")
    o.add_argument("a", type=Path)
    o.add_argument("b", type=Path)
    args = p.parse_args(argv)
    if args.cmd == "compare":
        return compare(args.a, args.b)
    if args.cmd == "objcmp":
        return objcmp(args.a, args.b)
    return 2


if __name__ == "__main__":
    sys.exit(main())
