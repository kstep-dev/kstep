"""Shared types and utilities for the kSTEP fuzzer."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import random
from typing import Mapping, Optional
from scripts.consts import FUZZ_DIR

Ops = list[tuple[int, int, int, int]]


@dataclass(frozen=True)
class CheckerStatus:
    work_conserving_broken: bool
    cfs_util_decay_broken: bool
    rt_util_decay_broken: bool

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
class MutationPivot:
    cmd_id: int
    signal_ids: list[int]
    kind: str = "coverage"
    special_score: float = 0.0
    times_selected: int = 0

    def weight(self, signal_test_counts: Mapping[int, int], rarity_alpha: float) -> float:
        from scripts.fuzz_mutate import pivot_weight

        return pivot_weight(
            signal_ids=self.signal_ids,
            signal_test_counts=signal_test_counts,
            rarity_alpha=rarity_alpha,
            special_score=self.special_score,
            times_selected=self.times_selected,
        )


@dataclass
class Seed:
    ops: Ops
    n_signals: int      # unique signals this seed first discovered
    seed_id: int
    times_selected: int = 0
    productive_pivots: list[MutationPivot] = field(default_factory=list)

    @property
    def productive_cmd_ids(self) -> list[int]:
        return [pivot.cmd_id for pivot in self.productive_pivots]


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
    mode: str = "fresh"        # "fresh" | "replay" | "mutate"
    seed_id: Optional[int] = None
    checker_status: Optional[CheckerStatus] = None
    special_pivot_idxs: list[int] = field(default_factory=list)
    error: Optional[str] = None
    error_category: Optional[str] = None  # "crash" | "timedout" | "retry_tick" | "op_mismatch" | "fail_log" | "other"


class SeedPool:
    def __init__(self) -> None:
        self._seeds: list[Seed] = []
        self._weights: list[int] = []
        self._counter = 0

    def next_seed_id(self) -> int:
        return self._counter
    
    def add(
        self,
        ops: Ops,
        n_signals: int,
        productive_pivots: Optional[list[MutationPivot]] = None,
        seed_id: Optional[int] = None,
    ) -> Seed:
        assigned_seed_id = self._counter if seed_id is None else seed_id
        seed = Seed(
            ops=ops,
            n_signals=n_signals,
            seed_id=assigned_seed_id,
            productive_pivots=productive_pivots or [],
        )
        self._seeds.append(seed)
        self._weights.append(max(1, seed.n_signals))
        self._counter = max(self._counter, assigned_seed_id + 1)
        return seed

    def pick_seed(self, rng: random.Random) -> "Optional[Seed]":
        """Pick a seed with probability proportional to its signal count."""
        if not self._seeds:
            return None
        seed = rng.choices(self._seeds, weights=self._weights, k=1)[0]
        seed.times_selected += 1
        return seed

    def pick_mutation_target(
        self,
        signal_test_counts: Mapping[int, int],
        rng: random.Random,
        rarity_alpha: float,
    ) -> Optional[tuple[Seed, MutationPivot]]:
        candidates: list[tuple[Seed, MutationPivot]] = []
        weights: list[float] = []

        for seed in self._seeds:
            for pivot in seed.productive_pivots:
                weight = pivot.weight(signal_test_counts, rarity_alpha)
                if weight <= 0:
                    continue
                candidates.append((seed, pivot))
                weights.append(weight)

        if not candidates:
            return None

        seed, pivot = rng.choices(candidates, weights=weights, k=1)[0]
        seed.times_selected += 1
        pivot.times_selected += 1
        return seed, pivot

    def __len__(self) -> int:
        return len(self._seeds)
