"""Shared types and utilities for the kSTEP fuzzer."""

from __future__ import annotations

import socket
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

Ops = list[tuple[int, int, int, int]]

# Marker byte written by kstep_write_state
OP_TYPE_NR = 16


def read_kmod_state(sf) -> Optional[list[dict]]:
    """Read state from kmod socket, discarding TTY echo.
    Returns list of {"id": int, "state": int} dicts, or None if socket closed."""
    while True:
        line = sf.readline()
        if not line:
            return None
        if line[0] == OP_TYPE_NR:
            payload = line[1:-1]  # strip marker byte and trailing '\n'
            return [{"id": payload[i], "state": payload[i + 1]}
                    for i in range(0, len(payload), 2)]


def connect_to_kmod(sock_file: Path, timeout: float = 10.0, retries: int = 200) -> socket.socket:

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sleep_time = timeout / retries
    for _ in range(retries):
        try:
            sock.connect(str(sock_file))
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(sleep_time)
    raise RuntimeError(f"Timed out connecting to {sock_file}")


@dataclass
class Seed:
    ops: Ops
    n_signals: int      # unique signals this seed first discovered
    seed_id: int
    times_replayed: int = 0


@dataclass
class WorkItem:
    mode: str           # "fresh" | "replay"
    steps: int          # used for "fresh"
    linux_name: str
    ops: Optional[Ops] = None     # used for "replay"
    seed_id: Optional[int] = None


@dataclass
class WorkResult:
    worker_id: int
    ops: Ops
    cov_file: Optional[Path]   # None when coverage is empty or on error
    log_file: Optional[Path]   # path to worker console log (for crash triage)
    debug_log_file: Optional[Path]   # path to worker debug log
    linux_name: str
    exec_time: float
    seed_id: Optional[int] = None
    error: Optional[str] = None
    crashed: bool = False      # True when kernel panic/BUG/KASAN detected


class SeedPool:
    def __init__(self) -> None:
        self._seeds: list[Seed] = []
        self._counter = 0
        self._next_idx = 0   # round-robin index

    def add(self, ops: Ops, n_signals: int) -> Seed:
        seed = Seed(ops=ops, n_signals=n_signals, seed_id=self._counter)
        self._seeds.append(seed)
        self._counter += 1
        return seed

    def __len__(self) -> int:
        return len(self._seeds)
