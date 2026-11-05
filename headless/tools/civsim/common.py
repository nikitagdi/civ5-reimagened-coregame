"""civsim.common — repo paths, hashing, canonical JSON, DLL/host/data resolution shared by simrunner, detlab and perflab."""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[3]
HEADLESS = REPO / "headless"
OUT = Path(os.environ["CIVSIM_OUT"]) if os.environ.get("CIVSIM_OUT") else HEADLESS / "out"    # CIVSIM_OUT: sandbox for tests
CORPORA = HEADLESS / "corpora"
DLL_NAME = "CvGameCore_Expansion2.dll"
DLL_VARIANTS = ("golden", "diag", "optimized", "optimized-diag", "profile")

_HASH_LOCK = threading.Lock()
_HASH_MEMO: dict[str, list] | None = None
_HASH_FILE: Path | None = None


# ---------------------------------------------------------------- json / files
def canonical_json(obj: Any) -> bytes:
    """Sorted-key, compact, UTF-8. Floats are rejected (they do not round-trip identically across tools)."""
    _reject_floats(obj, "$")
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def _reject_floats(obj: Any, where: str) -> None:
    if isinstance(obj, float):
        raise ValueError(f"{where}: floats are not allowed in sim fields (use integers or strings)")
    if isinstance(obj, dict):
        for k, v in obj.items():
            _reject_floats(v, f"{where}.{k}")
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _reject_floats(v, f"{where}[{i}]")


def write_json(path: Path, obj: Any) -> None:
    """Atomic JSON write (tmp + replace)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp{os.getpid()}-{threading.get_ident()}")
    tmp.write_text(json.dumps(obj, indent=2, sort_keys=False, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def read_json(path: Path) -> Any:
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)


def sha256_bytes(*chunks: bytes) -> str:
    h = hashlib.sha256()
    for c in chunks:
        h.update(c)
    return h.hexdigest()


def _memo_file() -> Path:
    return _HASH_FILE or (OUT / "runs" / "hashcache.json")


def file_sha256(path: Path | str) -> str:
    """sha256 of a file, memoized on (size, mtime_ns) in headless/out/runs/hashcache.json (the data DBs are ~50 MB)."""
    global _HASH_MEMO
    p = Path(path).resolve()
    st = p.stat()
    sig = [st.st_size, st.st_mtime_ns]
    with _HASH_LOCK:
        if _HASH_MEMO is None:
            try:
                _HASH_MEMO = read_json(_memo_file())
            except Exception:
                _HASH_MEMO = {}
        hit = _HASH_MEMO.get(str(p))
        if hit and hit[:2] == sig:
            return hit[2]
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    digest = h.hexdigest()
    with _HASH_LOCK:
        assert _HASH_MEMO is not None
        _HASH_MEMO[str(p)] = sig + [digest]
        try:
            write_json(_memo_file(), _HASH_MEMO)
        except OSError:
            pass
    return digest


def use_hash_memo(path: Path | None) -> None:
    """Redirect the file-hash memo (tests, selftest) so temp files do not pollute headless/out/runs/hashcache.json."""
    global _HASH_FILE, _HASH_MEMO
    _HASH_FILE, _HASH_MEMO = path, None


def utc_stamp() -> str:
    return time.strftime("%Y%m%d-%H%M%S", time.localtime())


# ---------------------------------------------------------------- DLL / host / data
@dataclass(frozen=True)
class Dll:
    label: str          # alias or file name, for reports
    path: Path
    sha256: str


def resolve_dll(spec: str) -> Dll:
    """golden|diag|optimized|optimized-diag|profile -> headless/out/dll/<variant>/CvGameCore_Expansion2.dll; otherwise a file path."""
    alias = OUT / "dll" / spec / DLL_NAME
    if not (os.sep in spec or "/" in spec or "\\" in spec) and alias.is_file():
        p, label = alias, spec
    elif Path(spec).is_file():
        p, label = Path(spec), Path(spec).parent.name or Path(spec).name
    else:
        raise FileNotFoundError(f"DLL '{spec}' not found (variant dir {alias} / file path)")
    p = p.resolve()
    return Dll(label=label, path=p, sha256=file_sha256(p))


@dataclass(frozen=True)
class Host:
    cmd: tuple[str, ...]    # argv prefix (python + script for the fake host)
    path: Path
    build_sha256: str


def resolve_host(spec: str | None = None) -> Host:
    """--host / $CIVHOST / headless/out/host/Release/civhost.exe. A .py file is run with this interpreter (fake host)."""
    spec = spec or os.environ.get("CIVHOST") or str(OUT / "host" / "Release" / "civhost.exe")
    p = Path(spec)
    if not p.is_file():
        raise FileNotFoundError(f"host binary not found: {p} (build with pwsh headless\\tools\\build-host.ps1)")
    p = p.resolve()
    cmd = (sys.executable, str(p)) if p.suffix.lower() == ".py" else (str(p),)
    return Host(cmd=cmd, path=p, build_sha256=file_sha256(p))


@dataclass(frozen=True)
class Data:
    path: Path
    hash: str


def resolve_data(spec: str | None = None) -> Data:
    """Data profile dir (Civ5DebugDatabase.db + Localization-Merged.db); default: newest headless/out/oracle/cache-*."""
    if spec:
        d = Path(spec)
    else:
        cands = sorted((OUT / "oracle").glob("cache-*"))
        if not cands:
            raise FileNotFoundError(f"no data profile under {OUT / 'oracle'}")
        d = cands[-1]
    d = d.resolve()
    parts = []
    for name in ("Civ5DebugDatabase.db", "Localization-Merged.db"):
        f = d / name
        if f.is_file():
            parts.append(name.encode() + b"\0" + file_sha256(f).encode())
    if not parts:
        raise FileNotFoundError(f"{d}: no Civ5DebugDatabase.db / Localization-Merged.db")
    return Data(path=d, hash=sha256_bytes(b"\n".join(parts)))


_FLAGS_MEMO: dict[tuple, frozenset[str]] = {}


def host_flags(host: Host) -> frozenset[str]:
    """Flags the host advertises in its usage text (`--help`), so tools can pass new flags only when they exist."""
    key = host.cmd
    if key not in _FLAGS_MEMO:
        flags: set[str] = set()
        try:
            r = subprocess.run([*host.cmd, "--help"], capture_output=True, timeout=30, text=True, errors="replace")
            import re
            flags = set(re.findall(r"(?<![\w-])--[a-z][a-z0-9-]*", r.stdout + r.stderr))
        except Exception:
            pass
        _FLAGS_MEMO[key] = frozenset(flags)
    return _FLAGS_MEMO[key]


# ---------------------------------------------------------------- Windows process helpers (best effort)
_JOB = None
_JOB_TRIED = False


def kill_on_close_job():
    """One Job Object (KILL_ON_JOB_CLOSE) for every host we start: killing the runner leaves no orphan civhost. None off Windows/on error."""
    global _JOB, _JOB_TRIED
    if _JOB_TRIED:
        return _JOB
    _JOB_TRIED = True
    if os.name != "nt":
        return None
    try:
        import ctypes
        from ctypes import wintypes

        class IO_COUNTERS(ctypes.Structure):
            _fields_ = [(n, ctypes.c_uint64) for n in ("r_ops", "w_ops", "o_ops", "r_bytes", "w_bytes", "o_bytes")]

        class BASIC(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                        ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

        class EXT(ctypes.Structure):
            _fields_ = [("Basic", BASIC), ("Io", IO_COUNTERS), ("ProcessMemoryLimit", ctypes.c_size_t),
                        ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                        ("PeakJobMemoryUsed", ctypes.c_size_t)]

        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateJobObjectW.restype = wintypes.HANDLE
        k32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
        k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
        job = k32.CreateJobObjectW(None, None)
        info = EXT()
        info.Basic.LimitFlags = 0x2000      # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if job and k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):
            _JOB = job
    except Exception:
        _JOB = None
    return _JOB


def attach_process(proc: subprocess.Popen, affinity_mask: int | None = None) -> None:
    """Put a started process into the kill-on-close job and optionally pin it to the logical CPUs in affinity_mask."""
    if os.name != "nt":
        return
    try:
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        k32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        h = int(proc._handle)  # type: ignore[attr-defined]
        job = kill_on_close_job()
        if job:
            k32.AssignProcessToJobObject(job, h)
        if affinity_mask:
            k32.SetProcessAffinityMask(h, affinity_mask)
    except Exception:
        pass
