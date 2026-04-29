import logging
import subprocess
from enum import StrEnum
from pathlib import Path
import json
from typing import TextIO


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


_COMMAND_LOG_PATH: Path | None = None


def set_command_log_path(path: Path | None):
    global _COMMAND_LOG_PATH
    _COMMAND_LOG_PATH = path


def get_command_log_path() -> Path | None:
    return _COMMAND_LOG_PATH


def append_log_line(line: str):
    if _COMMAND_LOG_PATH is None:
        return
    _COMMAND_LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with _COMMAND_LOG_PATH.open("a", encoding="utf-8") as f:
        f.write(line)
        if not line.endswith("\n"):
            f.write("\n")

def _open_command_log() -> TextIO | None:
    if _COMMAND_LOG_PATH is None:
        return None
    _COMMAND_LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    return _COMMAND_LOG_PATH.open("a", encoding="utf-8")


def system(cmd: str):
    logging.info(f"Running: `{TermColor.BLUE}{cmd}{TermColor.RESET}`")

    log_file = _open_command_log()
    if log_file is None:
        subprocess.run(cmd, shell=True, check=True)
    else:
        log_file.write(f"$ {cmd}\n")
        log_file.flush()
        subprocess.run(
            cmd,
            shell=True,
            check=True,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
        log_file.write("\n")
        log_file.flush()


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
