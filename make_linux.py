#!/usr/bin/env python3

import argparse
import shutil
from pathlib import Path

from scripts import LINUX_CONFIG, LINUX_CURR_DIR, PROJ_DIR, system

BEAR_CMD = f"bear --append --output {PROJ_DIR}/compile_commands.json --"


def make_linux(linux_dir: Path, clean: bool = False, reconfig: bool = False):
    # Clean up old build
    if clean:
        system(f"make -C {linux_dir} -j$(nproc) mrproper")

    # Generate config
    config_path = linux_dir / ".config"
    if not config_path.exists() or reconfig:
        system(f"make -C {linux_dir} -j$(nproc) defconfig")
        system(
            f"cd {linux_dir} && ./scripts/kconfig/merge_config.sh -m {config_path} {LINUX_CONFIG}"
        )
        system(f"make -C {linux_dir} -j$(nproc) olddefconfig")
        # mod2noconfig introduced in Linux 5.17 to avoid building modules
        # https://github.com/torvalds/linux/commit/c39afe624853e39af243dd9832640bf9c80b6554
        system(f"make -C {linux_dir} -j$(nproc) mod2noconfig || true")

    # Build kernel
    cmd = "make -j$(nproc) LOCALVERSION=-kstep WERROR=0"
    if shutil.which("bear"):
        cmd = f"{BEAR_CMD} {cmd}"
    cmd = f"cd {linux_dir} && {cmd}"
    system(cmd)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_dir", type=Path, default=LINUX_CURR_DIR)
    parser.add_argument("--clean", action="store_true", default=False)
    parser.add_argument("--reconfig", action="store_true", default=False)
    args = parser.parse_args()
    make_linux(**vars(args))
