import hashlib
from dataclasses import dataclass
from typing import Iterable, Iterator

OpTuple = tuple[int, int, int, int]


@dataclass
class InputSeq:
    _ops: list[OpTuple]

    def __init__(self, ops: Iterable[OpTuple] | None = None) -> None:
        self._ops = list(ops) if ops is not None else []

    def __len__(self) -> int:
        return len(self._ops)

    def __iter__(self) -> Iterator[OpTuple]:
        return iter(self._ops)

    def __getitem__(self, idx: int) -> OpTuple:
        return self._ops[idx]

    def __repr__(self) -> str:
        return repr(self._ops)

    def append(self, op: tuple[int, int, int, int]) -> None:
        self._ops.append((int(op[0]), int(op[1]), int(op[2]), int(op[3])))

    def to_payload(self) -> str:
        return "\n".join(f"{op},{a},{b},{c}" for op, a, b, c in self._ops) + "\n"

    def to_list(self) -> list[tuple[int, int, int, int]]:
        return self._ops

    def digest(self) -> str:
        return hashlib.sha256(self.to_payload().encode("utf-8")).hexdigest()

