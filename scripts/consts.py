import os
from datetime import datetime
from enum import Enum
from pathlib import Path

PROJ_DIR = Path(__file__).parent.parent.resolve()

LINUX_ROOT_DIR = PROJ_DIR / "linux"
LINUX_CONFIG = LINUX_ROOT_DIR / "config"
LINUX_MASTER_DIR = LINUX_ROOT_DIR / "master"
LINUX_CURR_DIR = LINUX_ROOT_DIR / "current"

USER_DIR = PROJ_DIR / "user"
KMOD_DIR = PROJ_DIR / "kmod"

DATA_DIR = PROJ_DIR / "data"
ROOTFS_IMG = DATA_DIR / "rootfs.cpio"
LOGS_DIR = DATA_DIR / "logs"



def get_log_path(create: bool) -> Path:
    symlink = LOGS_DIR / "latest.log"

    if create:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        actual = LOGS_DIR / f"log-{timestamp}.log"
        actual.touch()
        symlink.unlink(missing_ok=True)
        symlink.symlink_to(actual)
        return actual
    else:
        if symlink.exists():
            return symlink.resolve()
        else:
            return symlink



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
