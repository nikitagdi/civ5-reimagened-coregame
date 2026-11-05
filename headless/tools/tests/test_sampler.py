"""Unit tests of the pure parts of civsim.perflab.sampler (PE parsing, return-address validation, unwinder) and, on Windows, of the dbghelp
symbolizer. The sampling loop itself needs a live 32-bit civhost and is exercised by `python -m civsim.perflab sample ...`."""
from __future__ import annotations

import os
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from civsim.perflab import hotspots as H            # noqa: E402
from civsim.perflab import sampler as SM            # noqa: E402

BASE_A, BASE_B = 0x10000000, 0x20000000


def code_blob(size: int = 0x1000, calls: dict[int, bytes] | None = None) -> bytes:
    b = bytearray(b"\x90" * size)
    for off, ins in (calls or {}).items():
        b[off:off + len(ins)] = ins
    return bytes(b)


def make_map(fp_a: bool = True) -> SM.ModuleMap:
    mm = SM.ModuleMap(None)
    a = SM.Mod("cvgamecore_fake.dll", "a.dll", BASE_A, 0x2000)
    # call rel32 at 0x100 (+5 = ret 0x105) targeting 0x400; call eax at 0x300 (ret 0x302); call [ebx+8] at 0x500 (ret 0x503)
    rel = struct.pack("<i", 0x400 - 0x105)
    a.secs = [(0x1000, code_blob(0x1000, {0x100: b"\xe8" + rel, 0x300: b"\xff\xd0", 0x500: b"\xff\x53\x08"}))]
    a.fp = fp_a
    b = SM.Mod("lib_fpo.dll", "b.dll", BASE_B, 0x2000)
    b.secs = [(0x1000, code_blob(0x1000))]
    mm._add(a)
    mm._add(b)
    return mm


def rva(a_off: int) -> int:
    return BASE_A + 0x1000 + a_off


class TestParts(unittest.TestCase):
    def test_pe_sections(self):
        hdr = bytearray(0x400)
        hdr[:2] = b"MZ"
        struct.pack_into("<I", hdr, 0x3C, 0x80)
        hdr[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<H", hdr, 0x86, 2)            # NumberOfSections
        struct.pack_into("<H", hdr, 0x94, 0xE0)         # SizeOfOptionalHeader
        o = 0x80 + 24 + 0xE0
        struct.pack_into("<II", hdr, o + 8, 0x500, 0x1000)         # .text: vsize, rva
        struct.pack_into("<I", hdr, o + 36, 0x60000020)
        struct.pack_into("<II", hdr, o + 40 + 8, 0x200, 0x2000)    # .data
        struct.pack_into("<I", hdr, o + 40 + 36, 0xC0000040)
        self.assertEqual(SM.parse_pe_exec_sections(bytes(hdr)), [(0x1000, 0x500)])
        self.assertEqual(SM.parse_pe_exec_sections(b"not a pe" * 100), [])

    def test_is_call_ret(self):
        mm = make_map()
        self.assertTrue(mm.is_call_ret(rva(0x105)))            # E8 whose target (0x400) lies in a module
        self.assertTrue(mm.is_call_ret(rva(0x302)))            # call eax
        self.assertTrue(mm.is_call_ret(rva(0x503)))            # call [ebx+8]
        self.assertFalse(mm.is_call_ret(rva(0x700)))           # nops
        self.assertFalse(mm.is_call_ret(0x5000))               # outside every module
        # E8 whose target leaves the modules is rejected
        mm2 = make_map()
        mm2.mods[0].secs = [(0x1000, code_blob(0x1000, {0x100: b"\xe8" + struct.pack("<i", 0x40000000)}))]
        self.assertFalse(mm2.is_call_ret(rva(0x105)))

    def _stack(self, esp: int, words: dict[int, int], n: int = 64) -> bytes:
        w = [0] * n
        for addr, val in words.items():
            w[(addr - esp) // 4] = val
        return struct.pack("<%dI" % n, *w)

    def test_unwind_ebp_chain(self):
        mm = make_map()
        esp, ebp0, ebp1 = 0x1000, 0x1020, 0x1040
        win = self._stack(esp, {ebp0: ebp1, ebp0 + 4: rva(0x105), ebp1: 0, ebp1 + 4: rva(0x302)})
        frames, scans = SM.unwind(mm, rva(0x600), esp, ebp0, win)
        self.assertEqual(frames, [rva(0x600), rva(0x105), rva(0x302)])
        self.assertEqual(scans, 1)                       # the chain ends at ebp1 (saved ebp 0): one scan finds nothing more

    def test_unwind_prologue_and_ret(self):
        mm = make_map()
        esp, ebp = 0x1000, 0x1040
        a = mm.mods[0]
        a.secs = [(0x1000, code_blob(0x1000, {0x600: b"\x55\x8b\xec", 0x700: b"\xc3", 0x800: b"\x5d\xc3", 0x100: b"\xe8" + struct.pack("<i", 0x400 - 0x105)}))]
        # at `push ebp`: return address at [esp], the caller's frame pointer is still EBP
        win = self._stack(esp, {esp: rva(0x105), ebp + 4: rva(0x302)})
        frames, _ = SM.unwind(mm, rva(0x600), esp, ebp, win)
        self.assertEqual(frames[:2], [rva(0x600), rva(0x105)])
        # at `pop ebp` the saved ebp sits at [esp], the return address above it
        win = self._stack(esp, {esp: ebp, esp + 4: rva(0x105)})
        frames, _ = SM.unwind(mm, rva(0x800), esp, 0xdead, win)
        self.assertEqual(frames[:2], [rva(0x800), rva(0x105)])

    def test_unwind_scan_through_fpo_leaf(self):
        mm = make_map()
        esp = 0x1000
        junk = {esp + 4 * i: 0x00123456 + i for i in range(8)}       # locals that are not code addresses
        win = self._stack(esp, {**junk, esp + 40: rva(0x105), esp + 60: rva(0x302)})
        frames, scans = SM.unwind(mm, BASE_B + 0x1800, esp, 0, win)  # eip in the FPO module: frames found by scanning
        self.assertEqual(frames, [BASE_B + 0x1800, rva(0x105), rva(0x302)])
        self.assertGreaterEqual(scans, 2)
        # a repeat of the previous frame is skipped (stale slot)
        win = self._stack(esp, {esp + 8: rva(0x105), esp + 12: rva(0x105), esp + 16: rva(0x302)})
        frames, _ = SM.unwind(mm, BASE_B + 0x1800, esp, 0, win)
        self.assertEqual(frames, [BASE_B + 0x1800, rva(0x105), rva(0x302)])

    def test_category(self):
        self.assertEqual(H.category("CvGameCore_Expansion2.dll"), "gamecore DLL")
        self.assertEqual(H.category("civhost.exe"), "host (civhost.exe)")
        self.assertEqual(H.category("MSVCR90.dll", "C:\\Windows\\WinSxS\\x\\MSVCR90.dll"), "CRT")
        self.assertEqual(H.category("ntdll.dll", "C:\\Windows\\SYSTEM32\\ntdll.dll"), "OS")
        self.assertEqual(H.category("lua51_Win32.dll"), "lua51")
        self.assertEqual(H.category("CvGameDatabaseWin32Final Release.dll"), "database DLL")


@unittest.skipUnless(os.name == "nt" and Path(r"C:\Windows\SysWOW64\kernel32.dll").is_file(), "needs Windows with SysWOW64")
class TestSymbolizer(unittest.TestCase):
    def test_export_roundtrip(self):
        import ctypes
        path = r"C:\Windows\SysWOW64\kernel32.dll"
        raw = Path(path).read_bytes()
        e = struct.unpack_from("<I", raw, 0x3C)[0]
        size = struct.unpack_from("<I", raw, e + 24 + 56)[0]              # OptionalHeader.SizeOfImage
        base = 0x75000000
        sym = SM.Symbolizer([{"name": "KERNEL32.DLL", "path": path, "base": base, "size": size}])
        try:
            dh = sym.dh
            dh.SymFromName.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(SM.SYMBOL_INFO)]
            si = sym._si.contents
            si.SizeOfStruct, si.MaxNameLen = ctypes.sizeof(SM.SYMBOL_INFO), 1023
            self.assertTrue(dh.SymFromName(sym.HP, b"Sleep", sym._si))
            mod, name, start = sym.resolve(int(si.Address) + 2)
            self.assertEqual(mod, "KERNEL32.DLL")
            self.assertEqual(name, "Sleep")
            self.assertEqual(sym.resolve(0x1234)[0], "?")                  # outside every module
        finally:
            sym.close()


if __name__ == "__main__":
    unittest.main()
