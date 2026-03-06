#!/usr/bin/env -S uv run --script

import argparse
import json
from pathlib import Path

import pandas as pd

from utils import parse_line
from consts import LATEST_LOG


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
    parser.add_argument("path", type=Path, default=LATEST_LOG, nargs="?")
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
