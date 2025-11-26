#!/usr/bin/env python3

import argparse
import shutil
from pathlib import Path

from scripts import LINUX_CONFIG, LINUX_CURR_DIR, system

BEAR_CMD = "bear --append --output compile_commands.json --"


def make_linux(linux_dir: Path, clean: bool = False, reconfig: bool = False):
    # Clean up old build
    if clean:
        system(f"make -C {linux_dir} -j$(nproc) mrproper")

    # Generate config
    config_path = linux_dir / ".config"
    if not config_path.exists() or reconfig:
        system(f"cd {linux_dir} && ./scripts/kconfig/merge_config.sh -n {LINUX_CONFIG}")
    # Build kernel
    cmd = "make -j$(nproc) LOCALVERSION=-kstep WERROR=0"
    if shutil.which("bear"):
        cmd = f"{BEAR_CMD} {cmd}"
    cmd = f"cd {linux_dir} && KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' {cmd}"
    system(cmd)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_dir", type=Path, default=LINUX_CURR_DIR)
    parser.add_argument("--clean", action="store_true", default=False)
    parser.add_argument("--reconfig", action="store_true", default=False)
    args = parser.parse_args()
    make_linux(**vars(args))
