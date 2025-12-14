#!/usr/bin/env -S uv run --script

import argparse
import json
from pathlib import Path

import pandas as pd
from consts import LOG_LATEST

LOG_PREFIX_LEN = 15


def parse_json(prefix: str, path: Path) -> pd.DataFrame:
    data = []
    with open(path, "r") as f:
        for line in f:
            end = LOG_PREFIX_LEN + len(prefix)
            if line[LOG_PREFIX_LEN: end] != prefix:
                continue
            data.append(json.loads(line[end:]))
    return pd.DataFrame(data)


def parse_task(path: Path) -> pd.DataFrame:
    return parse_json("task: ", path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path, default=LOG_LATEST, nargs="?")
    args = parser.parse_args()
    df = parse_task(args.path)
    print(df)
