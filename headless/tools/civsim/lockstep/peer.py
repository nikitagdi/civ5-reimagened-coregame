"""One civhost peer process driven over --control stdio (JSON lines). Requests are pipelined: send() to every peer, then
read() every reply, so the peers compute their frame in parallel."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

from .. import common as C


class PeerError(RuntimeError):
    pass


class Peer:
    def __init__(self, pid: int, player: int | str, run_dir: Path, exe: Path, dll: Path, extra: list[str] | None = None):
        self.id = pid
        self.player = player
        self.run_dir = Path(run_dir)
        self.run_dir.mkdir(parents=True, exist_ok=True)
        args = [str(exe), "--control", "stdio", "--out", str(self.run_dir), "--dll", str(dll)] + list(extra or [])
        self.args = args
        self.stderr = open(self.run_dir / "stderr.txt", "wb")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr, cwd=str(C.REPO))
        C.attach_process(self.proc)
        self.turn = None
        self.digests: dict[int, str] = {}       # turn -> compared digest (masked when a mask is set) from step replies
        self.raw_digests: dict[int, str] = {}   # turn -> unmasked combined digest
        self.frames_in_turn = 0
        self.last = {}

    def send(self, cmd: str, **kw) -> None:
        kw["cmd"] = cmd
        try:
            self.proc.stdin.write((json.dumps(kw, separators=(",", ":")) + "\n").encode("utf-8"))
            self.proc.stdin.flush()
        except OSError as e:
            raise PeerError(f"peer {self.id}: write failed ({e}); exit {self.proc.poll()}")

    def read(self) -> dict:
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise PeerError(f"peer {self.id}: civhost closed the channel (exit {self.proc.poll()}); see {self.run_dir / 'civhost.log'}")
            msg = json.loads(line.decode("utf-8"))
            if "event" in msg:
                continue
            if not msg.get("ok"):
                raise PeerError(f"peer {self.id}: {msg.get('where')}: {msg.get('error')}")
            return msg

    def call(self, cmd: str, **kw) -> dict:
        self.send(cmd, **kw)
        return self.read()

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self.call("quit")
            except (PeerError, OSError, ValueError):
                pass
            try:
                self.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        try:
            self.stderr.close()
        except OSError:
            pass
