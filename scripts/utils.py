import json
import logging
import subprocess
from dataclasses import dataclass
from datetime import datetime
from enum import StrEnum
from pathlib import Path
from typing import Optional

PROJ_DIR = Path(__file__).parent.parent.resolve()

LINUX_ROOT_DIR = PROJ_DIR / "linux"
LINUX_CONFIG = LINUX_ROOT_DIR / "config"

RESULTS_DIR = PROJ_DIR / "results"
DATA_DIR = PROJ_DIR / "data"
BUILD_DIR = PROJ_DIR / "build"
BUILD_CURR_DIR = BUILD_DIR / "current"
LINUX_MASTER_DIR = BUILD_DIR / "master"

CORPUS_DIR = DATA_DIR / "corpus"

ts = datetime.now().strftime("%Y%m%d_%H%M%S")
FUZZ_DIR = RESULTS_DIR / f"fuzz_{ts}"
FUZZ_SUCCESS_DIR = FUZZ_DIR / "success"
FUZZ_ERROR_DIR = FUZZ_DIR / "error"
FUZZ_CORPUS_DIR = FUZZ_DIR / "corpus"


def get_build_dir(name: str) -> Path:
    return BUILD_DIR / name


def get_linux_dir(name: str) -> Path:
    return get_build_dir(name) / "linux"


def fuzz_mode_dir(mode: str) -> Path:
    name = {
        "fresh": "fresh",
        "replay": "replay",
        "mutate": "mutation",
    }.get(mode, mode)
    return RESULTS_DIR / "fuzz" / name


@dataclass(frozen=True)
class ResultDir:
    """A per-run directory under RESULTS_DIR with stable child file names."""
    name: str

    @classmethod
    def create(cls, name: Optional[str] = None, set_latest: bool = True) -> "ResultDir":
        """Create `results/<name>/` (defaults to `tmp_<ts>`); optionally point `results/latest` at it."""
        if name is None:
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            name = f"tmp_{ts}"
        out = cls(name)
        out.path.mkdir(parents=True, exist_ok=True)
        if set_latest:
            latest = RESULTS_DIR / "latest"
            latest.unlink(missing_ok=True)
            latest.symlink_to(name)
        return out

    def __str__(self) -> str: return str(self.path)

    @property
    def path(self) -> Path: return RESULTS_DIR / self.name
    @property
    def log(self) -> Path: return self.path / "qemu.log"
    @property
    def output(self) -> Path: return self.path / "kstep.jsonl"
    @property
    def cov(self) -> Path: return self.path / "kstep.cov"
    @property
    def sock(self) -> Path: return self.path / "qemu.sock"
    @property
    def debug_log(self) -> Path: return self.path / "debug.log"


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
    logging.info(f"$ {TermColor.BLUE}{cmd}{TermColor.RESET}")
    subprocess.run(cmd, shell=True, check=True, cwd=PROJ_DIR)


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
