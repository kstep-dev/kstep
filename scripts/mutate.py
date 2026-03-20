"""Mutation strategy: replay a seed up to a productive command, then generate interactively."""

from __future__ import annotations

import random
from typing import Optional


def pick_pivot(productive_cmd_ids: list[int], rng: random.Random) -> Optional[int]:
    """Randomly select a cmd_id that previously found a new signal as the pivot point.

    The worker will replay ops[0..pivot_idx] to reconstruct GenState at that
    context, then generate fresh commands interactively from that point forward.

    Returns None if there are no productive commands (caller should fall back to replay).
    """
    if not productive_cmd_ids:
        return None
    return rng.choice(productive_cmd_ids)
