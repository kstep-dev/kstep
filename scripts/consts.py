import os
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Optional

PROJ_DIR = Path(__file__).parent.parent.resolve()

LINUX_ROOT_DIR = PROJ_DIR / "linux"
LINUX_CONFIG = LINUX_ROOT_DIR / "config"

USER_DIR = PROJ_DIR / "user"
KMOD_DIR = PROJ_DIR / "kmod"

DATA_DIR = PROJ_DIR / "data"
ROOTFS_IMG = DATA_DIR / "rootfs.ext4"
LOGS_DIR = DATA_DIR / "logs"


def get_log_path(create: bool):
    if create:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        for suffix in [".log", ".json", ".csv", ".txt"]:
            actual = LOGS_DIR / f"log-{timestamp}{suffix}"
            actual.touch()
            symlink = LOGS_DIR / f"latest{suffix}"
            symlink.unlink(missing_ok=True)
            symlink.symlink_to(actual)

    return LOGS_DIR / "latest.log"


def get_linux_dir(version: Optional[str] = None):
    if version is None:
        return LINUX_ROOT_DIR / "current"
    return LINUX_ROOT_DIR / f"linux-{version}"

class Arch(Enum):
    X86_64 = "x86_64"
    ARM64 = "arm64"

    @classmethod
    def get(cls):
        machine = os.uname().machine
        if machine == "x86_64":
            return cls.X86_64
        elif machine == "aarch64":
            return cls.ARM64
        else:
            raise ValueError(f"Unsupported architecture: {machine}")
