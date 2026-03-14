"""Shared types for the kSTEP fuzzer (worker ↔ manager IPC)."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Optional

Ops = list[tuple[int, int, int, int]]


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
    ops: Ops = None     # used for "replay"
    seed_id: Optional[int] = None


@dataclass
class WorkResult:
    worker_id: int
    ops: Ops
    cov_file: Optional[Path]   # None when coverage is empty or on error
    log_file: Optional[Path]   # path to worker console log (for crash triage)
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

    def pick_next(self) -> Optional[Seed]:
        """Round-robin over all seeds."""
        if not self._seeds:
            return None
        seed = self._seeds[self._next_idx % len(self._seeds)]
        self._next_idx += 1
        seed.times_replayed += 1
        return seed

    def __len__(self) -> int:
        return len(self._seeds)
