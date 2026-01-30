#!/usr/bin/env -S uv run --script

import argparse
import json
from pathlib import Path

import pandas as pd
from consts import LOG_LATEST, RESULTS_DIR

TIMESTAMP_LEN = 14


def parse_line(line: str, prefix: str) -> dict | None:
    # Line format: [timestamp] prefix: {json}
    if len(line) < TIMESTAMP_LEN:
        return None
    if line[0] != "[" or line[TIMESTAMP_LEN - 1] != "]":
        return None
    if not line.startswith(prefix, TIMESTAMP_LEN + 1):
        return None

    # Parse JSON
    json_str = line[TIMESTAMP_LEN + 1 + len(prefix) :].removeprefix(":")
    try:
        obj = json.loads(json_str)
    except json.JSONDecodeError:
        raise ValueError(f"Invalid JSON at line {line}")

    # Parse timestamp
    ts_str = line[1 : TIMESTAMP_LEN - 1]
    ts = round((float(ts_str) - 10) * 1000)

    # Add timestamp to object
    obj["timestamp"] = ts
    return obj


def parse_log(path: Path, prefix: str) -> pd.DataFrame:
    print(f'Parsing {path} with prefix "{prefix}"')
    data = []
    with open(path, "r") as f:
        for line in f:
            obj = parse_line(line, prefix)
            if obj is None:
                continue
            data.append(obj)
    if not data:
        print(f'No data found for {path} with prefix "{prefix}"')
    return pd.DataFrame(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path, default=LOG_LATEST, nargs="?")
    parser.add_argument(
        "--prefixes",
        type=str,
        default=["task", "rq", "load_balance", "nr_running", "sched_softirq"],
        nargs="*",
    )
    args = parser.parse_args()
    for prefix in args.prefixes:
        df = parse_log(args.path, prefix)
        with pd.option_context(
            "display.max_rows", None, "display.max_columns", None, "display.width", None
        ):
            print(df)
        print("-" * 80)
