#!/usr/bin/env -S uv run --script

import argparse
import json
import re
from pathlib import Path

from consts import OUT_LATEST


def parse_line(line: str) -> dict | None:
    # There is a kernel output bug that produces unmatched braces.
    # Replace array fields (type[])... with empty list
    # by finding the last ']' before the next '.'.
    suffix = lambda s: s[s.rfind("]") :]
    line = re.sub(
        r"\([^()]*\[\]\)[^.]*(?=\.|$)", lambda m: "[" + suffix(m.group()), line
    )
    # Replace pointer fields (type *)... with null
    line = re.sub(r"\([^()]+\*\)[^\s,]+", "null", line)
    # Add placeholder names for anonymous unions and structs
    line = re.sub(r"(?<![=] )\(union\)\{", r".union = {", line)
    line = re.sub(r"(?<![=] )\(struct\)\{", r".struct = {", line)
    # Remove (type) before values
    line = re.sub(r"\([^()]*\)", "", line)
    # Convert hex values to decimal
    line = re.sub(r"\b0x([0-9a-fA-F]+)\b", lambda m: str(int(m.group(1), 16)), line)
    # Convert .field = to "field":
    line = re.sub(r"\.(\w+) = ", r'"\1":', line)
    # Remove trailing commas before } or ]
    line = re.sub(r",([}\]])", r"\1", line)

    return json.loads(line)


def parse_out(path: Path) -> list[dict]:
    data = []
    with open(path, "r") as f:
        for line in f:
            obj = parse_line(line)
            data.append(obj)
    print(f"Parsed {len(data)} lines from {path}")
    return data


def main(path: Path):
    data = parse_out(path)
    jsonl_path = path.with_suffix(".jsonl")
    with open(jsonl_path, "w") as f:
        for obj in data:
            f.write(json.dumps(obj) + "\n")
    print(f"Saved {jsonl_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path, default=OUT_LATEST, nargs="?")
    args = parser.parse_args()
    main(args.path)
