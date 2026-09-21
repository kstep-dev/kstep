"""What the plot scripts share: where results live, and how to read a trace."""

import json
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

RESULTS_DIR = Path(__file__).parent.parent.resolve() / "results"


@dataclass(frozen=True)
class ResultDir:
    """`results/<name>/`, as `kstep run` and `kstep reproduce` lay it out."""
    name: str

    @property
    def path(self) -> Path: return RESULTS_DIR / self.name
    @property
    def log(self) -> Path: return self.path / "qemu.log"
    @property
    def output(self) -> Path: return self.path / "kstep.jsonl"


def parse_jsonl(path: Path, type: str) -> pd.DataFrame:
    """The records of one `type` in a kstep.jsonl trace, without the type column."""
    rows = []
    with open(path) as f:
        for line in f:
            record = json.loads(line)
            if record.get("type") == type:
                del record["type"]
                rows.append(record)
    return pd.DataFrame(rows)
