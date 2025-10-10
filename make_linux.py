#!/usr/bin/env python3

import argparse
from pathlib import Path

from scripts import LINUX_CONFIG, get_linux_dir, system

BEAR_CMD = "bear --append --output compile_commands.json --"

def make_linux(linux_dir: Path, clean: bool = False):
    # Clean up old build
    if clean:
        system(f"make -C {linux_dir} -j$(nproc) mrproper")

    # Generate config
    if not (linux_dir / ".config").exists():
        system(f"make -C {linux_dir} -j$(nproc) defconfig")
        system(
            f"cd {linux_dir} && {linux_dir}/scripts/kconfig/merge_config.sh {linux_dir}/.config {LINUX_CONFIG}"
        )

    # Build kernel
    system(f"{BEAR_CMD} make -C {linux_dir} -j$(nproc)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--clean", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--versions", nargs="+", type=str, default=["6.14"])
    args = parser.parse_args()
    for version in args.versions:
        make_linux(linux_dir=get_linux_dir(version), clean=args.clean)
