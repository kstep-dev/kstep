#!/usr/bin/env python3

import argparse
from pathlib import Path

from scripts import LINUX_CONFIG, PROJ_DIR, get_linux_dir, system

BEAR_CMD = f"bear --append --output {PROJ_DIR}/compile_commands.json --"


def make_linux(linux_dir: Path, clean: bool = False, modules_prepare: bool = False):
    # Clean up old build
    if clean:
        system(f"make -C {linux_dir} -j$(nproc) mrproper")

    # Generate config
    config_path = linux_dir / ".config"
    if not config_path.exists():
        system(f"make -C {linux_dir} -j$(nproc) defconfig")
        system(
            f"cd {linux_dir} && ./scripts/kconfig/merge_config.sh -m {config_path} {LINUX_CONFIG}"
        )
        system(f"make -C {linux_dir} -j$(nproc) olddefconfig")

    # Build kernel
    if modules_prepare:
        system(f"make -C {linux_dir} -j$(nproc) modules_prepare")
    else:
        system(f"{BEAR_CMD} make -C {linux_dir} -j$(nproc)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--versions", nargs="+", type=str, default=["6.14"])
    parser.add_argument("--clean", action="store_true", default=False)
    parser.add_argument("--modules_prepare", action="store_true", default=False)
    args = parser.parse_args()
    for version in args.versions:
        make_linux(
            linux_dir=get_linux_dir(version),
            clean=args.clean,
            modules_prepare=args.modules_prepare,
        )
