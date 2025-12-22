#!/usr/bin/env -S uv run --script

import argparse
import json
from pathlib import Path

import pandas as pd
from consts import LOG_LATEST

LOG_PREFIX_LEN = 14


def parse_line(prefix: str, line: str) -> list:
    # Line format: [timestamp] prefix: {json}
    if len(line) < LOG_PREFIX_LEN:
        return []
    if line[0] != "[" or line[LOG_PREFIX_LEN - 1] != "]":
        return []
    if not line.startswith(prefix, LOG_PREFIX_LEN + 1):
        return []

    # Parse JSON
    json_str = line[LOG_PREFIX_LEN + 1 + len(prefix) :]
    try:
        obj = json.loads(json_str)
    except json.JSONDecodeError:
        raise ValueError(f"Invalid JSON at line {line}")

    # Parse timestamp
    ts_str = line[1 : LOG_PREFIX_LEN - 1]
    ts = round((float(ts_str) - 10) * 1000)

    # Add timestamp to object
    if isinstance(obj, dict):
        obj["timestamp"] = ts
        return [obj]
    elif isinstance(obj, list):
        for item in obj:
            item["timestamp"] = ts
        return obj
    else:
        raise ValueError(f"Invalid object: {obj}")


def parse_file(prefix: str, path: Path) -> pd.DataFrame:
    data = []
    with open(path, "r") as f:
        for line in f:
            data.extend(parse_line(prefix, line))
    return pd.DataFrame(data)


def parse_task(path: Path) -> pd.DataFrame:
    print(f"Parsing task from {path}")
    return parse_file("task: ", path)


def parse_rq(path: Path) -> pd.DataFrame:
    print(f"Parsing rq from {path}")
    return parse_file("rq: ", path)


def parse_load_balance(path: Path) -> pd.DataFrame:
    print(f"Parsing load_balance from {path}")
    return parse_file("load_balance: ", path)


def parse_nr_running(path: Path) -> pd.DataFrame:
    print(f"Parsing nr_running from {path}")
    return parse_file("nr_running: ", path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path, default=LOG_LATEST, nargs="?")
    args = parser.parse_args()
    for fn in [parse_task, parse_rq, parse_load_balance, parse_nr_running]:
        print(f"Parsing {fn.__name__} from {args.path}")
        df = fn(args.path)
        with pd.option_context(
            "display.max_rows", None, "display.max_columns", None, "display.width", None
        ):
            print(df)
        print("-" * 80)
