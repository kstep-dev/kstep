"""What the plot scripts share: where results live, and how to read a trace."""

import json
from pathlib import Path

import pandas as pd

RESULTS_DIR = Path(__file__).parent.parent.resolve() / "results"


def kstep_log(name: str) -> Path:
    """`results/<name>/kstep.jsonl`: the driver's records, as `kstep run` and `kstep reproduce` lay them out."""
    return RESULTS_DIR / name / "kstep.jsonl"


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
