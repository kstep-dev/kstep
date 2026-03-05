import hashlib
from dataclasses import dataclass
from typing import Iterable, Iterator
from pathlib import Path
from .utils import parse_line

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

def input_seq_from_log(log_file: Path) -> InputSeq:
    seq = InputSeq()
    with open(log_file, "r", encoding="utf-8") as f:
        for line in f:
            parts = parse_line(line, "EXECOP")
            if parts is None:
                continue
            seq.append((int(parts["op"]), int(parts["a"]), int(parts["b"]), int(parts["c"])))
    return seq

