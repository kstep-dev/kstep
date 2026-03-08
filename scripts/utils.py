import logging
import subprocess
from enum import StrEnum
from pathlib import Path
import json


class TermColor(StrEnum):
    GRAY = "\033[90m"
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    MAGENTA = "\033[95m"
    CYAN = "\033[96m"
    WHITE = "\033[97m"
    RESET = "\033[0m"


def system(cmd: str):
    logging.info(f"Running: `{TermColor.BLUE}{cmd}{TermColor.RESET}`")
    subprocess.run(cmd, shell=True, check=True)

def download(url: str, output_path: Path):
    if output_path.exists():
        logging.info(f"File {output_path} already exists, skipping download")
        return
    system(f"wget --no-verbose {url} -O {output_path}")


def decompress(tarball_path: Path, output_dir: Path):
    if output_dir.exists():
        logging.info(f"Directory {output_dir} already exists, skipping decompression")
        return
    system(f"mkdir -p {output_dir}")
    system(f"tar -xf {tarball_path} -C {output_dir} --strip-components=1")

TIMESTAMP_LEN = 14

# Parse a line from the log file
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
