#!/usr/bin/env -S uv run --script
import argparse
import json
import logging
from pathlib import Path

import pandas as pd

from scripts import get_log_path


def parse_file(log_file: Path):
    data = {}
    with open(log_file, "r") as f:
        for line in f:
            if len(line) < 15:
                continue
            if line[0] != "[" or line[15] != "{":
                continue

            if "QEMU: Terminated" in line:
                break

            timestamp = float(line[1:13])
            json_str = line[15:]
            json_obj = json.loads(json_str)
            if timestamp not in data:
                data[timestamp] = {"timestamp": timestamp}
            data[timestamp] |= json_obj
    result = list(data.values())
    result.pop()  # remove the last item as it might be incomplete
    return result


def main(log_file: Path):
    data = parse_file(log_file)
    logging.info(f"Parsed {log_file}")

    json_path = log_file.with_suffix(".json")
    with open(json_path, "w") as f:
        json.dump(data, f, indent=2)
    logging.info(f"Saved {json_path}")

    df = pd.json_normalize(data)

    csv_path = log_file.with_suffix(".csv")
    df.to_csv(csv_path, index=False)
    logging.info(f"Saved {csv_path}")

    txt_path = log_file.with_suffix(".txt")
    df.to_string(txt_path)
    logging.info(f"Saved {txt_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--log_file", type=Path, default=get_log_path(create=False))
    args = parser.parse_args()

    main(**vars(args))
