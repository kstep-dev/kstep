#!/usr/bin/env -S uv run --script

import argparse
import json
from pathlib import Path

import pandas as pd
from consts import LOG_LATEST

LOG_PREFIX_LEN = 14


def parse_file(prefix: str, path: Path) -> pd.DataFrame:
    data = []
    with open(path, "r") as f:
        for line in f:
            # Check format
            if len(line) < LOG_PREFIX_LEN:
                continue
            if line[0] != "[" or line[LOG_PREFIX_LEN - 1] != "]":
                continue
            if not line.startswith(prefix, LOG_PREFIX_LEN + 1):
                continue

            # Parse JSON
            json_str = line[LOG_PREFIX_LEN + 1 + len(prefix) :]
            obj = json.loads(json_str)

            # Parse timestamp
            ts_str = line[1 : LOG_PREFIX_LEN - 1]
            ts = round((float(ts_str) - 10) * 1000)

            # Append to data
            if isinstance(obj, dict):
                obj["timestamp"] = ts
                data.append(obj)
            elif isinstance(obj, list):
                for item in obj:
                    item["timestamp"] = ts
                    data.append(item)
            else:
                raise ValueError(f"Invalid object: {obj}")

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
