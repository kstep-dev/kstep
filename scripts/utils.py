import logging
import subprocess
from enum import StrEnum

from .consts import PROJ_DIR


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


def make_rootfs():
    system(f"make -C {PROJ_DIR} -j$(nproc) rootfs")
