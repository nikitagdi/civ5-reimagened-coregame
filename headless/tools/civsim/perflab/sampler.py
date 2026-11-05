"""sampler: external sampling profiler for civhost (Windows only; 64-bit Python samples the 32-bit civhost main thread).

Sampling: Wow64SuspendThread + Wow64GetThreadContext(WOW64_CONTEXT, CONTEXT_CONTROL) + one ReadProcessMemory of a stack window
(from ESP), then ResumeThread; the stack is unwound afterwards from the copied window. Unwinding is a hybrid: an EBP chain where the
code keeps frame pointers (the `profile` DLL variant is built /Oy-, civhost.exe is /Oy-), otherwise (CRT, lua, sqlite, OS) a scan of
the stack window for words that are return addresses (the bytes in front of the address must be a `call`, a rel32 call must land in a
known module). Return addresses are validated against code snapshots read once per module from the target.

Offline: addresses -> functions through dbghelp (ctypes; dummy process handle, SymLoadModuleEx at the runtime base with the PDB next
to the DLL). Modules without PDB resolve to their exports (nearest export if close, else a 4 KB bucket of the module).

Outputs in headless/out/perf/<name>/: samples.json.gz (raw: modules, unique stacks, per-sample stack id + time), hotspots.json/.md.
"""
from __future__ import annotations

import bisect
import ctypes
import gzip
import json
import os
import re
import struct
import subprocess
import sys
import time
from collections import Counter, defaultdict
from ctypes import wintypes as W
from pathlib import Path
from typing import Any, Callable

from .. import common as C

# ---------------------------------------------------------------- Win32
if os.name == "nt":
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    winmm = ctypes.WinDLL("winmm")
else:                                                   # import must not fail off Windows (unit tests of the pure parts)
    k32 = psapi = winmm = None

PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, SYNCHRONIZE = 0x0400, 0x0010, 0x00100000
THREAD_SUSPEND_RESUME, THREAD_GET_CONTEXT, THREAD_QUERY_INFORMATION = 0x0002, 0x0008, 0x0040
CONTEXT_i386 = 0x00010000
CONTEXT_CONTROL = CONTEXT_i386 | 0x1
CREATE_SUSPENDED = 0x00000004
WAIT_OBJECT_0 = 0


class WOW64_FLOATING_SAVE_AREA(ctypes.Structure):
    _fields_ = [("ControlWord", W.DWORD), ("StatusWord", W.DWORD), ("TagWord", W.DWORD), ("ErrorOffset", W.DWORD),
                ("ErrorSelector", W.DWORD), ("DataOffset", W.DWORD), ("DataSelector", W.DWORD), ("RegisterArea", ctypes.c_ubyte * 80),
                ("Cr0NpxState", W.DWORD)]


class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [("ContextFlags", W.DWORD), ("Dr0", W.DWORD), ("Dr1", W.DWORD), ("Dr2", W.DWORD), ("Dr3", W.DWORD), ("Dr6", W.DWORD),
                ("Dr7", W.DWORD), ("FloatSave", WOW64_FLOATING_SAVE_AREA), ("SegGs", W.DWORD), ("SegFs", W.DWORD), ("SegEs", W.DWORD),
                ("SegDs", W.DWORD), ("Edi", W.DWORD), ("Esi", W.DWORD), ("Ebx", W.DWORD), ("Edx", W.DWORD), ("Ecx", W.DWORD),
                ("Eax", W.DWORD), ("Ebp", W.DWORD), ("Eip", W.DWORD), ("SegCs", W.DWORD), ("EFlags", W.DWORD), ("Esp", W.DWORD),
                ("SegSs", W.DWORD), ("ExtendedRegisters", ctypes.c_ubyte * 512)]


class WOW64_LDT_ENTRY(ctypes.Structure):
    _fields_ = [("LimitLow", W.WORD), ("BaseLow", W.WORD), ("BaseMid", ctypes.c_ubyte), ("Flags1", ctypes.c_ubyte), ("Flags2", ctypes.c_ubyte),
                ("BaseHi", ctypes.c_ubyte)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD), ("th32OwnerProcessID", W.DWORD),
                ("tpBasePri", W.LONG), ("tpDeltaPri", W.LONG), ("dwFlags", W.DWORD)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", W.DWORD), ("EntryPoint", ctypes.c_void_p)]


assert ctypes.sizeof(WOW64_CONTEXT) == 716


def _setup_api() -> None:
    if k32 is None:
        return
    H, D, B = W.HANDLE, W.DWORD, W.BOOL
    k32.OpenProcess.restype, k32.OpenProcess.argtypes = H, [D, B, D]
    k32.OpenThread.restype, k32.OpenThread.argtypes = H, [D, B, D]
    k32.CloseHandle.argtypes = [H]
    k32.Wow64SuspendThread.restype, k32.Wow64SuspendThread.argtypes = D, [H]
    k32.Wow64GetThreadContext.restype, k32.Wow64GetThreadContext.argtypes = B, [H, ctypes.POINTER(WOW64_CONTEXT)]
    k32.ResumeThread.restype, k32.ResumeThread.argtypes = D, [H]
    k32.Wow64GetThreadSelectorEntry.restype = B
    k32.Wow64GetThreadSelectorEntry.argtypes = [H, D, ctypes.POINTER(WOW64_LDT_ENTRY)]
    k32.ReadProcessMemory.restype = B
    k32.ReadProcessMemory.argtypes = [H, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    k32.CreateWaitableTimerExW.restype, k32.CreateWaitableTimerExW.argtypes = H, [ctypes.c_void_p, W.LPCWSTR, D, D]
    k32.SetWaitableTimer.restype = B
    k32.SetWaitableTimer.argtypes = [H, ctypes.POINTER(ctypes.c_longlong), W.LONG, ctypes.c_void_p, ctypes.c_void_p, B]
    k32.WaitForSingleObject.restype, k32.WaitForSingleObject.argtypes = D, [H, D]
    k32.GetThreadTimes.restype = B
    k32.GetThreadTimes.argtypes = [H] + [ctypes.POINTER(W.FILETIME)] * 4
    k32.GetExitCodeProcess.restype, k32.GetExitCodeProcess.argtypes = B, [H, ctypes.POINTER(D)]
    k32.CreateToolhelp32Snapshot.restype, k32.CreateToolhelp32Snapshot.argtypes = H, [D, D]
    k32.Thread32First.restype = k32.Thread32Next.restype = B
    k32.Thread32First.argtypes = k32.Thread32Next.argtypes = [H, ctypes.POINTER(THREADENTRY32)]
    psapi.EnumProcessModulesEx.restype = B
    psapi.EnumProcessModulesEx.argtypes = [H, ctypes.POINTER(ctypes.c_void_p), D, ctypes.POINTER(D), D]
    psapi.GetModuleFileNameExW.restype, psapi.GetModuleFileNameExW.argtypes = D, [H, ctypes.c_void_p, W.LPWSTR, D]
    psapi.GetModuleInformation.restype = B
    psapi.GetModuleInformation.argtypes = [H, ctypes.c_void_p, ctypes.POINTER(MODULEINFO), D]


_setup_api()


def _ft(ft: "W.FILETIME") -> int:
    return (ft.dwHighDateTime << 32) | ft.dwLowDateTime


def thread_times(h: int) -> tuple[int, int]:
    """(creation FILETIME, cpu 100ns = kernel + user) of a thread handle."""
    c, e, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
    k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    return _ft(c), _ft(kt) + _ft(ut)


def list_threads(pid: int) -> list[int]:
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    if not snap or snap == ctypes.c_void_p(-1).value:
        return []
    tids = []
    try:
        te = THREADENTRY32()
        te.dwSize = ctypes.sizeof(te)
        ok = k32.Thread32First(snap, ctypes.byref(te))
        while ok:
            if te.th32OwnerProcessID == pid:
                tids.append(te.th32ThreadID)
            ok = k32.Thread32Next(snap, ctypes.byref(te))
    finally:
        k32.CloseHandle(snap)
    return tids


def open_thread(tid: int, access: int = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION) -> int | None:
    return k32.OpenThread(access, False, tid) or None


def main_thread(pid: int) -> int:
    """Thread id of the earliest-created thread of the process."""
    best, best_t = None, None
    for tid in list_threads(pid):
        h = open_thread(tid, THREAD_QUERY_INFORMATION)
        if not h:
            continue
        created, _ = thread_times(h)
        k32.CloseHandle(h)
        if best_t is None or created < best_t:
            best, best_t = tid, created
    if best is None:
        raise OSError(f"no thread found for pid {pid}")
    return best


# ---------------------------------------------------------------- module map (runtime)
class Mod:
    __slots__ = ("name", "path", "base", "size", "end", "fp", "secs", "prologue_ratio")

    def __init__(self, name: str, path: str, base: int, size: int):
        self.name, self.path, self.base, self.size, self.end = name, path, base, size, base + size
        self.fp = False                 # code keeps EBP frames (/Oy-)
        self.secs: list[tuple[int, bytes]] = []     # (rva, bytes) of executable sections
        self.prologue_ratio = 0.0

    def sec(self, rva: int) -> tuple[int, bytes] | None:
        for r0, data in self.secs:
            if r0 <= rva < r0 + len(data):
                return r0, data
        return None


def parse_pe_exec_sections(hdr: bytes) -> list[tuple[int, int]]:
    """[(rva, virtual size)] of executable sections from the first page(s) of a PE image."""
    if len(hdr) < 0x100 or hdr[:2] != b"MZ":
        return []
    e_lfanew = struct.unpack_from("<I", hdr, 0x3C)[0]
    if e_lfanew + 24 > len(hdr) or hdr[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return []
    nsec, = struct.unpack_from("<H", hdr, e_lfanew + 6)
    optsz, = struct.unpack_from("<H", hdr, e_lfanew + 20)
    off = e_lfanew + 24 + optsz
    out = []
    for i in range(nsec):
        o = off + 40 * i
        if o + 40 > len(hdr):
            break
        vsize, vaddr = struct.unpack_from("<II", hdr, o + 8)
        chars, = struct.unpack_from("<I", hdr, o + 36)
        if chars & 0x20000000 or chars & 0x20:           # MEM_EXECUTE or CNT_CODE
            out.append((vaddr, vsize))
    return out


class ModuleMap:
    """Loaded 32-bit modules of the target with code snapshots; answers find / is_call_ret / code_at."""

    def __init__(self, hproc: int | None = None, fp_modules: str = "auto"):
        self.hproc = hproc
        self.mods: list[Mod] = []
        self.bases: list[int] = []
        self.lo, self.hi = 1 << 32, 0
        self._cr: dict[int, bool] = {}
        self.fp_modules = fp_modules            # auto | comma list of module names | none
        self.last_refresh = 0.0

    # --- runtime enumeration
    def refresh(self) -> bool:
        """Re-enumerate; returns True if modules were added."""
        self.last_refresh = time.perf_counter()
        n = 512
        arr = (ctypes.c_void_p * n)()
        need = W.DWORD()
        if not psapi.EnumProcessModulesEx(self.hproc, arr, ctypes.sizeof(arr), ctypes.byref(need), 0x03):
            return False
        known = {m.base for m in self.mods}
        added = False
        for i in range(min(n, need.value // ctypes.sizeof(ctypes.c_void_p))):
            hm = arr[i]
            mi = MODULEINFO()
            if not hm or not psapi.GetModuleInformation(self.hproc, hm, ctypes.byref(mi), ctypes.sizeof(mi)):
                continue
            base = mi.lpBaseOfDll or 0
            if base >= (1 << 32) or base in known:
                continue                            # 64-bit (wow64) modules are not part of a 32-bit thread's stack
            buf = ctypes.create_unicode_buffer(520)
            psapi.GetModuleFileNameExW(self.hproc, hm, buf, 520)
            m = Mod(os.path.basename(buf.value), buf.value, base, mi.SizeOfImage)
            self._snapshot(m)
            self._add(m)
            added = True
        return added

    def _read(self, addr: int, n: int) -> bytes:
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        k32.ReadProcessMemory(self.hproc, addr, buf, n, ctypes.byref(got))
        return buf.raw[:got.value]

    def _snapshot(self, m: Mod) -> None:
        hdr = self._read(m.base, 0x1000)
        for rva, vsize in parse_pe_exec_sections(hdr):
            data = self._read(m.base + rva, vsize)
            if data:
                m.secs.append((rva, data))
        code = sum(len(d) for _, d in m.secs) or 1
        pro = sum(d.count(b"\x55\x8b\xec") for _, d in m.secs)
        m.prologue_ratio = pro / code
        self._decide_fp(m)

    def _decide_fp(self, m: Mod) -> None:
        if self.fp_modules == "none":
            m.fp = False
        elif self.fp_modules == "auto":
            low = m.name.lower()
            m.fp = low == "civhost.exe" or (low.startswith("cvgamecore") and m.prologue_ratio > 1 / 600)
        else:
            m.fp = m.name.lower() in {x.strip().lower() for x in self.fp_modules.split(",")}

    def _add(self, m: Mod) -> None:
        i = bisect.bisect_left(self.bases, m.base)
        self.bases.insert(i, m.base)
        self.mods.insert(i, m)
        self.lo, self.hi = min(self.lo, m.base), max(self.hi, m.end)

    # --- queries (used by the unwinder; also by tests with a fake map)
    def find(self, addr: int) -> Mod | None:
        i = bisect.bisect_right(self.bases, addr) - 1
        if i >= 0 and addr < self.mods[i].end:
            return self.mods[i]
        return None

    def code_at(self, addr: int, n: int) -> bytes:
        m = self.find(addr)
        if m is None:
            return b""
        s = m.sec(addr - m.base)
        if s is None:
            return b""
        o = addr - m.base - s[0]
        return s[1][o:o + n]

    def is_call_ret(self, r: int) -> bool:
        """True if r is a plausible return address: the instruction before it is a call."""
        hit = self._cr.get(r)
        if hit is not None:
            return hit
        m = self.find(r)
        if m is None:
            return False
        s = m.sec(r - m.base)
        ok = False
        if s is not None:
            b, o = s[1], r - m.base - s[0]
            if o >= 5 and b[o - 5] == 0xE8:
                tgt = (r + struct.unpack_from("<i", b, o - 4)[0]) & 0xFFFFFFFF
                ok = self.find(tgt) is not None
            if not ok and o >= 2:
                x1, x2 = b[o - 2], b[o - 1]
                if x1 == 0xFF and (0xD0 <= x2 <= 0xD7 or (0x10 <= x2 <= 0x17 and x2 not in (0x14, 0x15))):
                    ok = True                                                   # call reg / call [reg]
                elif o >= 3 and b[o - 3] == 0xFF and (0x50 <= x1 <= 0x57 and x1 != 0x54 or x1 == 0x14):
                    ok = True                                                   # call [reg+d8] / call [sib]
                elif o >= 4 and b[o - 4] == 0xFF and b[o - 3] == 0x54:
                    ok = True                                                   # call [sib+d8]
                elif o >= 6 and b[o - 6] == 0xFF and (b[o - 5] == 0x15 or 0x90 <= b[o - 5] <= 0x97 and b[o - 5] != 0x94):
                    ok = True                                                   # call [abs32] / call [reg+d32]
                elif o >= 7 and b[o - 7] == 0xFF and b[o - 6] in (0x94, 0x14):
                    ok = True                                                   # call [sib+d32]
        self._cr[r] = ok
        return ok


# ---------------------------------------------------------------- unwinder
_U32 = struct.Struct("<I").unpack_from
MAX_SCAN0 = 1024        # bytes scanned above ESP for the first return address of a frameless (non-/Oy-) leaf


def unwind(mods, eip: int, esp: int, ebp: int, win: bytes, max_depth: int = 48) -> tuple[list[int], int]:
    """frames[0] = eip, then return addresses innermost first. Second result: number of scan steps (frames not found by a frame chain)."""
    n = len(win)
    lim = esp + n
    lo, hi = mods.lo, mods.hi

    def rd(a: int) -> int | None:
        o = a - esp
        if o < 0 or o + 4 > n:
            return None
        return _U32(win, o)[0]

    def scan(frm: int, max_bytes: int, skip: int = -1) -> tuple[int | None, int | None]:
        end = min(lim - 3, frm + max_bytes)
        a = frm
        while a + 4 <= end:
            cnt = min(96, (end - a) // 4)               # chunked: the first hit is usually close
            words = struct.unpack_from("<%dI" % cnt, win, a - esp)
            for i, w in enumerate(words):
                if lo <= w < hi and w != skip and mods.is_call_ret(w):
                    return w, a + 4 * i
            a += 4 * cnt
        return None, None

    frames = [eip]
    scans = 0
    ret = ret_at = None
    extra_fp: int | None = None         # frame pointer of the caller when the current function has no frame yet/anymore
    m = mods.find(eip)
    if m is not None and m.fp:
        code = mods.code_at(eip, 3)
        c0 = code[:1]
        if c0 == b"\x55" or c0 == b"\xc3" or c0 == b"\xc2" or c0 == b"\xe9" or code[:2] == b"\xff\x25":
            ret, ret_at, extra_fp = rd(esp), esp, ebp          # before push ebp / at ret / jmp thunk: EBP is still the caller's
        elif code[:2] == b"\x8b\xec" or c0 == b"\x5d":
            ret, ret_at = rd(esp + 4), esp + 4                 # after push ebp, before mov ebp,esp / at pop ebp
        else:
            ret, ret_at = rd(ebp + 4), ebp + 4
        if ret is None or not (lo <= ret < hi and mods.is_call_ret(ret)):
            ret = ret_at = extra_fp = None
    if ret is None:
        ret, ret_at = scan(esp, MAX_SCAN0)
        scans += 1
        if ret is not None:
            extra_fp = ebp
    while ret is not None and len(frames) < max_depth:
        frames.append(ret)
        nret = nret_at = None
        for fp in (extra_fp, rd(ret_at - 4)):
            if fp is not None and fp > ret_at and not fp & 3:
                r2 = rd(fp + 4)
                if r2 is not None and lo <= r2 < hi and mods.is_call_ret(r2):
                    nret, nret_at = r2, fp + 4
                    break
        extra_fp = None
        if nret is None:
            nret, nret_at = scan(ret_at + 4, 1 << 20, ret)       # skip a repeat of the last frame (stale slot)
            scans += 1
        ret, ret_at = nret, nret_at
    return frames, scans


# ---------------------------------------------------------------- sampler
class Sampler:
    def __init__(self, hproc: int, pid: int, hz: int = 1000, stack_bytes: int = 16384, max_depth: int = 64, fp_modules: str = "auto",
                 log: Callable[[str], None] | None = None):
        self.hproc, self.pid, self.hz = hproc, pid, hz
        self.stack_bytes, self.max_depth = stack_bytes, max_depth
        self.log = log or (lambda m: None)
        self.mods = ModuleMap(hproc, fp_modules)
        self.stacks: dict[tuple, int] = {}
        self.seq: list[int] = []
        self.t_us: list[int] = []
        self.stats: dict[str, Any] = {}
        self.threads: dict[int, int] = {}       # tid -> cpu 100ns (last seen)
        self.main_tid: int | None = None
        self.interrupted = False

    def _thread_cpu_snapshot(self) -> None:
        for tid in list_threads(self.pid):
            h = open_thread(tid, THREAD_QUERY_INFORMATION)
            if h:
                self.threads[tid] = thread_times(h)[1]
                k32.CloseHandle(h)

    def _stack_bounds(self, hthread: int) -> tuple[int, int] | None:
        """(stack limit, stack base) of the WOW64 thread from its 32-bit TEB (FS selector 0x53); None until the thread has initialized."""
        ldt = WOW64_LDT_ENTRY()
        if not k32.Wow64GetThreadSelectorEntry(hthread, 0x53, ctypes.byref(ldt)):
            return None
        teb = ldt.BaseLow | (ldt.BaseMid << 16) | (ldt.BaseHi << 24)
        raw = self.mods._read(teb + 4, 8) if teb else b""
        if len(raw) < 8:
            return None
        base, limit = struct.unpack("<II", raw)
        return (limit, base) if 0 < limit < base < (1 << 31) and base - limit <= (256 << 20) else None

    def run(self, hthread: int, duration_s: float | None = None) -> dict:
        """Sample until the process exits (or duration). hthread: handle with SUSPEND_RESUME|GET_CONTEXT|QUERY_INFORMATION."""
        period_ms = max(1, round(1000 / self.hz))
        winmm.timeBeginPeriod(1)
        timer = k32.CreateWaitableTimerExW(None, None, 0x2, 0x1F0003)       # CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        if not timer:
            timer = k32.CreateWaitableTimerExW(None, None, 0, 0x1F0003)
        due = ctypes.c_longlong(-10000 * period_ms)
        k32.SetWaitableTimer(timer, ctypes.byref(due), period_ms, None, None, False)
        ctx = WOW64_CONTEXT()
        ctx.ContextFlags = CONTEXT_CONTROL
        cref = ctypes.byref(ctx)
        nbuf = self.stack_bytes
        buf = ctypes.create_string_buffer(nbuf)
        got = ctypes.c_size_t()
        gref = ctypes.byref(got)
        suspend, getctx, resume, rpm, wait = (k32.Wow64SuspendThread, k32.Wow64GetThreadContext, k32.ResumeThread, k32.ReadProcessMemory,
                                              k32.WaitForSingleObject)
        string_at, pc = ctypes.string_at, time.perf_counter_ns
        stacks, seq, t_us, mods = self.stacks, self.seq, self.t_us, self.mods
        maxd = self.max_depth
        fail = scans_total = frames_total = trunc = nomod = 0
        susp_ns = 0
        bounds: tuple[int, int] | None = None
        t0 = pc()
        last_thr = last_mod = t0
        t_end = t0 + int(duration_s * 1e9) if duration_s else None
        mods.refresh()
        tick = 0
        try:
            while True:
                wait(timer, 1000)
                now = pc()
                if t_end and now > t_end:
                    break
                tick += 1
                if tick & 63 == 0 and wait(self.hproc, 0) == WAIT_OBJECT_0:
                    break
                if now - last_mod > (1e8 if now - t0 < 5e9 else 1e9):             # modules load early (delay-loaded DLLs): poll fast at first
                    last_mod = now
                    mods.refresh()
                if now - last_thr > 2e9:
                    last_thr = now
                    self._thread_cpu_snapshot()
                a = pc()
                if suspend(hthread) == 0xFFFFFFFF:
                    if wait(self.hproc, 0) == WAIT_OBJECT_0:
                        break
                    fail += 1
                    continue
                ok = getctx(hthread, cref)
                n = 0
                if ok:
                    want = nbuf
                    if bounds is not None and bounds[0] <= ctx.Esp < bounds[1]:
                        want = min(nbuf, bounds[1] - ctx.Esp)           # never read past the stack base
                    if rpm(self.hproc, ctx.Esp, buf, want, gref) or got.value:
                        n = got.value
                    if bounds is None and tick & 127 == 0:
                        bounds = self._stack_bounds(hthread)
                resume(hthread)
                susp_ns += pc() - a
                if not ok or n < 8:
                    fail += 1
                    continue
                win = string_at(buf, n)
                frames, sc = unwind(mods, ctx.Eip, ctx.Esp, ctx.Ebp, win, maxd)
                scans_total += sc
                frames_total += len(frames)
                if len(frames) >= maxd:
                    trunc += 1
                if mods.find(frames[0]) is None:
                    nomod += 1
                key = tuple(frames)
                sid = stacks.get(key)
                if sid is None:
                    sid = stacks[key] = len(stacks)
                seq.append(sid)
                t_us.append((a - t0) // 1000)
        except KeyboardInterrupt:                       # Ctrl+C ends the sampling, the samples so far are kept
            self.interrupted = True
        finally:
            for _ in range(8):                            # never leave the target suspended
                if resume(hthread) <= 1:
                    break
        k32.CloseHandle(timer)
        winmm.timeEndPeriod(1)
        wall = (pc() - t0) / 1e9
        ns = max(1, len(seq))
        self._thread_cpu_snapshot()
        self.stats = {
            "hz_target": self.hz, "samples": len(seq), "failed": fail, "wall_s": round(wall, 3), "achieved_hz": round(len(seq) / wall, 1) if wall else 0,
            "mean_suspend_us": round(susp_ns / 1000 / max(1, len(seq) + fail), 1), "suspended_frac": round(susp_ns / 1e9 / wall, 4) if wall else 0,
            "unique_stacks": len(stacks), "mean_depth": round(frames_total / ns, 2), "scan_steps_per_sample": round(scans_total / ns, 3),
            "depth_capped": trunc, "eip_outside_modules": nomod, "stack_bytes": self.stack_bytes, "max_depth": maxd,
            "stack_bounds": [hex(bounds[0]), hex(bounds[1])] if bounds else None,
            "modules": [{"name": m.name, "path": m.path, "base": m.base, "size": m.size, "fp": m.fp, "prologue_ratio": round(m.prologue_ratio, 5)}
                        for m in mods.mods],
            "threads_cpu_s": {str(t): round(c / 1e7, 3) for t, c in sorted(self.threads.items())},
            "main_tid": self.main_tid, "interrupted": self.interrupted,
        }
        return self.stats

    def save(self, path: Path) -> None:
        stacks = [None] * len(self.stacks)
        for k, v in self.stacks.items():
            stacks[v] = list(k)
        doc = {"schema": 1, "stats": self.stats, "modules": self.stats["modules"], "stacks": stacks, "seq": self.seq, "t_us": self.t_us}
        path.parent.mkdir(parents=True, exist_ok=True)
        with gzip.open(path, "wt", encoding="utf-8", compresslevel=6) as f:
            json.dump(doc, f, separators=(",", ":"))


# ---------------------------------------------------------------- launch / attach
def launch_suspended(argv: list[str], stdout_path: Path, cwd: str | None = None) -> tuple[subprocess.Popen, int, int]:
    """Start argv suspended inside the kill-on-close job; returns (proc, main thread handle, tid). The caller resumes the thread."""
    log = open(stdout_path, "wb")
    proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=CREATE_SUSPENDED, cwd=cwd)
    C.attach_process(proc)
    tid = main_thread(proc.pid)
    h = open_thread(tid)
    if not h:
        proc.kill()
        raise OSError(f"cannot open main thread {tid}: {ctypes.get_last_error()}")
    return proc, h, tid


def sample_process(proc: subprocess.Popen, hthread: int, tid: int, hz: int, stack_bytes: int, max_depth: int, fp_modules: str,
                   log: Callable[[str], None]) -> Sampler:
    hproc = int(proc._handle)       # type: ignore[attr-defined]
    s = Sampler(hproc, proc.pid, hz, stack_bytes, max_depth, fp_modules, log)
    s.main_tid = tid
    k32.ResumeThread(hthread)       # CREATE_SUSPENDED: start the host; sampling begins at once
    s.run(hthread)
    return s


def attach_sample(pid: int, hz: int, stack_bytes: int, max_depth: int, fp_modules: str, duration_s: float | None, log: Callable[[str], None]) -> Sampler:
    hproc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, False, pid)
    if not hproc:
        raise OSError(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")
    tid = main_thread(pid)
    h = open_thread(tid)
    if not h:
        raise OSError(f"OpenThread({tid}) failed: {ctypes.get_last_error()}")
    s = Sampler(hproc, pid, hz, stack_bytes, max_depth, fp_modules, log)
    s.main_tid = tid
    s.run(h, duration_s)
    return s


# ---------------------------------------------------------------- dbghelp symbolization
class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.DWORD), ("TypeIndex", W.DWORD), ("Reserved", ctypes.c_uint64 * 2), ("Index", W.DWORD), ("Size", W.DWORD),
                ("ModBase", ctypes.c_uint64), ("Flags", W.DWORD), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64),
                ("Register", W.DWORD), ("Scope", W.DWORD), ("Tag", W.DWORD), ("NameLen", W.DWORD), ("MaxNameLen", W.DWORD),
                ("Name", ctypes.c_char * 1)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", W.DWORD), ("FileName", ctypes.c_char_p),
                ("Address", ctypes.c_uint64)]


class Symbolizer:
    """addr -> (module, function) offline. Loads each module at its runtime base into a dummy dbghelp process."""
    SYMOPT = 0x2 | 0x4 | 0x10 | 0x200 | 0x80000 | 0x2000   # UNDNAME | DEFERRED | LOAD_LINES | FAIL_CRITICAL | NO_PROMPTS | INCLUDE_32BIT
    HP = 0x5E57

    def __init__(self, modules: list[dict]):
        self.dh = ctypes.WinDLL("dbghelp", use_last_error=True)
        dh = self.dh
        dh.SymSetOptions.argtypes = [W.DWORD]
        dh.SymInitialize.argtypes = [ctypes.c_void_p, ctypes.c_char_p, W.BOOL]
        dh.SymLoadModuleEx.restype = ctypes.c_uint64
        dh.SymLoadModuleEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64, W.DWORD, ctypes.c_void_p, W.DWORD]
        dh.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
        dh.SymGetLineFromAddr64.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(W.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]
        dh.UnDecorateSymbolName.argtypes = [ctypes.c_char_p, ctypes.c_char_p, W.DWORD, W.DWORD]
        dh.SymSetOptions(self.SYMOPT)
        if not dh.SymInitialize(self.HP, None, False):
            raise OSError(f"SymInitialize failed: {ctypes.get_last_error()}")
        self.mods = sorted(modules, key=lambda m: m["base"])
        self.bases = [m["base"] for m in self.mods]
        self.pdb_hits: Counter = Counter()
        self.sym_hits: Counter = Counter()
        for m in self.mods:
            nm = m["name"].rsplit(".", 1)[0]
            r = dh.SymLoadModuleEx(self.HP, None, m["path"].encode("mbcs", "replace"), nm.encode(), m["base"], m["size"], None, 0)
            m["sym_loaded"] = bool(r)
        sz = ctypes.sizeof(SYMBOL_INFO)
        self._buf = ctypes.create_string_buffer(sz + 1024)
        self._si = ctypes.cast(self._buf, ctypes.POINTER(SYMBOL_INFO))
        self._cache: dict[int, tuple[str, str, int]] = {}
        self._out = ctypes.create_string_buffer(1024)

    def module_of(self, addr: int) -> dict | None:
        i = bisect.bisect_right(self.bases, addr) - 1
        if i >= 0 and addr < self.mods[i]["base"] + self.mods[i]["size"]:
            return self.mods[i]
        return None

    def _clean(self, name: str) -> str:
        if name.startswith("?"):
            n = self.dh.UnDecorateSymbolName(name.encode(), self._out, 1024, 0x1000)       # UNDNAME_NAME_ONLY
            if n:
                name = self._out.value.decode(errors="replace")
        elif name.startswith(("_", "@")) and "@" in name[1:] and name[1:].split("@")[-1].isdigit():
            name = name[1:].rsplit("@", 1)[0]               # stdcall/fastcall decoration
        return name[:160]

    def resolve(self, addr: int, is_return: bool = False) -> tuple[str, str, int]:
        """(module, function, function start address). Return addresses are looked up at addr-1."""
        key = addr - 1 if is_return else addr
        hit = self._cache.get(key)
        if hit is not None:
            return hit
        m = self.module_of(key)
        if m is None:
            res = ("?", f"0x{addr:08x}", addr)
        else:
            si = self._si.contents
            si.SizeOfStruct, si.MaxNameLen = ctypes.sizeof(SYMBOL_INFO), 1023
            disp = ctypes.c_uint64()
            mod = m["name"]
            rva = key - m["base"]
            if self.dh.SymFromAddr(self.HP, key, ctypes.byref(disp), self._si):
                off = SYMBOL_INFO.Name.offset
                name = self._buf.raw[off:off + si.NameLen].decode(errors="replace")
                is_export = bool(si.Flags & 0x200)          # SYMFLAG_EXPORT: no PDB, nearest export
                self.sym_hits["export" if is_export else "pdb"] += 1
                self.pdb_hits[mod, "export" if is_export else "pdb"] += 1
                if is_export and disp.value > 0x400:           # no PDB: only trust an export that is close, else a 1 KB bucket
                    res = (mod, f"{mod}+0x{rva & ~0x3FF:x}", key - (rva & 0x3FF))
                else:
                    nm = self._clean(name)
                    res = (mod, ("~" + nm) if is_export and disp.value > 0x80 else nm, int(si.Address))     # ~ = nearest export, approximate
            else:
                self.pdb_hits[mod, "none"] += 1
                res = (mod, f"{mod}+0x{rva & ~0x3FF:x}", key - (rva & 0x3FF))
        self._cache[key] = res
        return res

    def line(self, addr: int) -> str | None:
        ln = IMAGEHLP_LINE64()
        ln.SizeOfStruct = ctypes.sizeof(ln)
        d = W.DWORD()
        if self.dh.SymGetLineFromAddr64(self.HP, addr, ctypes.byref(d), ctypes.byref(ln)) and ln.FileName:
            return f"{os.path.basename(ln.FileName.decode(errors='replace'))}:{ln.LineNumber}"
        return None

    def close(self) -> None:
        self.dh.SymCleanup(self.HP)
