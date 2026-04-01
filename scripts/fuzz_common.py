"""Shared types and utilities for the kSTEP fuzzer."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import random
from typing import Optional
from scripts.consts import FUZZ_DIR

Ops = list[tuple[int, int, int, int]]

@dataclass(frozen=True)
class WorkerPaths:
    log_file: Path
    sock_file: Path
    debug_log_file: Path
    output_file: Path
    cov_file: Path


def worker_paths(worker_id: int, base_dir: Path = FUZZ_DIR) -> WorkerPaths:
    return WorkerPaths(
        log_file = base_dir / f"worker_{worker_id}.log",
        sock_file = base_dir / f"worker_{worker_id}.sock",
        debug_log_file = base_dir / f"worker_{worker_id}.debug.log",
        output_file = base_dir / f"worker_{worker_id}.jsonl",
        cov_file = base_dir / f"worker_{worker_id}.cov",
    )

@dataclass
class Seed:
    ops: Ops
    n_signals: int      # unique signals this seed first discovered
    seed_id: int
    times_replayed: int = 0
    productive_cmd_ids: list[int] = field(default_factory=list)  # cmd_ids that found new signals


@dataclass
class WorkItem:
    mode: str           # "fresh" | "replay" | "mutate"
    steps: int          # used for "fresh" and for interactive generation in "mutate"
    ops: Optional[Ops] = None         # used for "replay" / "mutate"
    seed_id: Optional[int] = None
    pivot_idx: Optional[int] = None   # "mutate": replay ops[0..pivot_idx], then generate interactively


@dataclass
class WorkResult:
    worker_id: int
    ops: Ops
    exec_time: float
    mode: str = "fresh"        # "fresh" | "replay"
    seed_id: Optional[int] = None
    error: Optional[str] = None
    error_category: Optional[str] = None  # "crash" | "timedout" | "retry_tick" | "op_mismatch" | "fail_log" | "other"


class SeedPool:
    def __init__(self) -> None:
        self._seeds: list[Seed] = []
        self._weights: list[int] = []
        self._counter = 0

    def next_seed_id(self) -> int:
        return self._counter
    
    def add(self, ops: Ops, n_signals: int, productive_cmd_ids: Optional[list[int]] = None) -> Seed:
        seed = Seed(
            ops=ops,
            n_signals=n_signals,
            seed_id=self._counter,
            productive_cmd_ids=productive_cmd_ids or [],
        )
        self._seeds.append(seed)
        self._weights.append(max(1, seed.n_signals))
        self._counter += 1
        return seed

    def pick_seed(self) -> "Optional[Seed]":
        """Pick a seed with probability proportional to its signal count."""
        if not self._seeds:
            return None
        seed = random.choices(self._seeds, weights=self._weights, k=1)[0]
        seed.times_replayed += 1
        return seed

    def __len__(self) -> int:
        return len(self._seeds)
