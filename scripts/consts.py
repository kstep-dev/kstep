import os
from enum import Enum
from pathlib import Path
from typing import Optional

PROJ_DIR = Path(__file__).parent.parent.resolve()

LINUX_ROOT_DIR = PROJ_DIR / "linux"
LINUX_CONFIG = LINUX_ROOT_DIR / "config"

USER_DIR = PROJ_DIR / "user"
KMOD_DIR = PROJ_DIR / "kmod"

ROOTFS_DIR = PROJ_DIR / "rootfs"
ROOTFS_IMG = ROOTFS_DIR / "img.ext4"

DATA_DIR = PROJ_DIR / "data"
LOG_PATH = DATA_DIR / "log.txt"


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
