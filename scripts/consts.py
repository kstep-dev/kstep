from pathlib import Path

PROJ_DIR = Path(__file__).parent.parent.resolve()

LINUX_VERSIONS_DIR = PROJ_DIR / "linux"
LINUX_DIR = LINUX_VERSIONS_DIR / "current"
LINUX_CUSTOM_CONFIG = LINUX_VERSIONS_DIR / "config"

USER_DIR = PROJ_DIR / "user"
KMOD_DIR = PROJ_DIR / "kmod"

ROOTFS_DIR = PROJ_DIR / "rootfs"
ROOTFS_IMG = ROOTFS_DIR / "img.ext4"
