"""Mutation operators for the kSTEP fuzzer."""

from __future__ import annotations

import random

from scripts.fuzz_common import Ops
from scripts.gen_input_ops import OP_NAME_TO_TYPE

_TICK = OP_NAME_TO_TYPE["TICK"]


def mutate_insert_ticks(ops: Ops, rng: random.Random, max_len: int) -> Ops:
    """Insert small bursts of TICKs just before non-TICK ops.

    Skips insertion when the sequence is already at or above max_len, preventing
    unbounded growth across successive mutation rounds.
    """
    if not ops or len(ops) >= max_len:
        return list(ops)

    candidates = [i for i, (op, *_) in enumerate(ops) if op != _TICK]
    if not candidates:
        candidates = [rng.randint(0, len(ops) - 1)]

    n_sites = rng.randint(1, min(3, len(candidates)))
    sites = set(rng.sample(candidates, n_sites))

    result: Ops = []
    for i, op_tuple in enumerate(ops):
        if i in sites:
            result.extend([(_TICK, 0, 0, 0)] * rng.randint(1, 3))
        result.append(op_tuple)
    return result


def mutate_delete_ticks(ops: Ops, rng: random.Random) -> Ops:
    """Delete ticks from runs of consecutive TICKs.

    Finds runs of 2+ consecutive TICKs, picks 1–3 of them, and removes a
    random portion of each run while keeping at least one TICK per run.
    This counteracts length growth from repeated tick insertion.
    """
    if not ops:
        return ops

    # Locate runs of 2+ consecutive TICKs.
    runs: list[tuple[int, int]] = []  # (start, end) — end is exclusive
    i = 0
    while i < len(ops):
        if ops[i][0] == _TICK:
            j = i + 1
            while j < len(ops) and ops[j][0] == _TICK:
                j += 1
            if j - i >= 2:
                runs.append((i, j))
            i = j
        else:
            i += 1

    if not runs:
        return list(ops)

    n_runs = rng.randint(1, min(3, len(runs)))
    chosen = rng.sample(runs, n_runs)

    to_delete: set[int] = set()
    for start, end in chosen:
        run_len = end - start
        n_delete = rng.randint(1, run_len - 1)  # always keep at least one
        to_delete.update(rng.sample(range(start, end), n_delete))

    return [op for i, op in enumerate(ops) if i not in to_delete]


def mutate(ops: Ops, rng: random.Random, max_len: int) -> Ops:
    """Choose randomly between tick insertion and tick deletion.

    Deletion is preferred when the sequence is at or above max_len so that
    long seeds are naturally trimmed back toward a healthy length.
    """
    if len(ops) >= max_len:
        return mutate_delete_ticks(ops, rng)
    if rng.random() < 0.5:
        return mutate_insert_ticks(ops, rng, max_len)
    return mutate_delete_ticks(ops, rng)
