import os
from enum import Enum
from pathlib import Path

PROJ_DIR = Path(__file__).parent.parent.resolve()

LINUX_ROOT_DIR = PROJ_DIR / "linux"
LINUX_CONFIG = LINUX_ROOT_DIR / "config"
LINUX_MASTER_DIR = LINUX_ROOT_DIR / "master"
LINUX_CURR_DIR = LINUX_ROOT_DIR / "current"
LINUX_BUILD_DIR = LINUX_ROOT_DIR / "build"

USER_DIR = PROJ_DIR / "user"
KMOD_DIR = PROJ_DIR / "kmod"

DATA_DIR = PROJ_DIR / "data"
DOWNLOAD_DIR = DATA_DIR / "download"
ROOTFS_DIR = DATA_DIR / "rootfs"
LOGS_DIR = DATA_DIR / "logs"
LOG_LATEST = LOGS_DIR / "latest.log"
OUT_LATEST = LOGS_DIR / "latest.out"
RESULTS_DIR = PROJ_DIR / "results"
QEMU_DIR = DATA_DIR / "qemu"


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
