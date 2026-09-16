"""Mutation strategy: replay a seed up to a productive command, then generate interactively."""

from __future__ import annotations

import math
from collections.abc import Mapping


def signal_rarity(
    sig: int,
    signal_test_counts: Mapping[int, int],
    rarity_alpha: float,
) -> float:
    if rarity_alpha <= 0:
        return 1.0
    return (1.0 / max(1, signal_test_counts.get(sig, 0))) ** rarity_alpha


def bottleneck_signal(
    signal_ids: list[int],
    signal_test_counts: Mapping[int, int],
    rarity_alpha: float,
) -> int | None:
    if not signal_ids:
        return None
    return min(
        signal_ids,
        key=lambda sig: (signal_rarity(sig, signal_test_counts, rarity_alpha), sig),
    )


def pivot_weight(
    signal_ids: list[int],
    signal_test_counts: Mapping[int, int],
    rarity_alpha: float,
    special_score: float,
    times_selected: int,
) -> float:
    """Score one mutation pivot by its bottleneck signal rarity and a small reuse decay."""
    if signal_ids:
        bottleneck = bottleneck_signal(signal_ids, signal_test_counts, rarity_alpha)
        assert bottleneck is not None
        rarity = signal_rarity(bottleneck, signal_test_counts, rarity_alpha)
        return rarity / math.sqrt(1.0 + times_selected)

    if special_score <= 0:
        return 0.0
    return special_score / math.sqrt(1.0 + times_selected)

