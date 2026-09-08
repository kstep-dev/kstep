from collections.abc import Iterable, Iterator
from dataclasses import dataclass
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

    def append(self, op: OpTuple) -> None:
        self._ops.append((int(op[0]), int(op[1]), int(op[2]), int(op[3])))

    def to_list(self) -> list[OpTuple]:
        return self._ops

def input_seq_from_log(log_file: Path) -> InputSeq:
    seq = InputSeq()
    with open(log_file, encoding="utf-8") as f:
        for line in f:
            parts = parse_line(line, "EXECOP")
            if parts is None:
                continue
            seq.append((int(parts["op"]), int(parts["a"]), int(parts["b"]), int(parts["c"])))
    return seq

