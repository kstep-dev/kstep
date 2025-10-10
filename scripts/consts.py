from pathlib import Path
from typing import Optional

PROJ_DIR = Path(__file__).parent.parent.resolve()

LINUX_ROOT_DIR = PROJ_DIR / "linux"
LINUX_CURR_DIR = LINUX_ROOT_DIR / "current"
LINUX_CONFIG = LINUX_ROOT_DIR / "config"

USER_DIR = PROJ_DIR / "user"
KMOD_DIR = PROJ_DIR / "kmod"

ROOTFS_DIR = PROJ_DIR / "rootfs"
ROOTFS_IMG = ROOTFS_DIR / "img.ext4"

def get_linux_dir(version: Optional[str] = None):
    if version is None:
        return LINUX_CURR_DIR
    return LINUX_ROOT_DIR / f"linux-{version}"
