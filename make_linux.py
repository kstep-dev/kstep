#!/usr/bin/env python3

import argparse

from scripts import LINUX_CUSTOM_CONFIG, LINUX_DIR, system

BEAR_CMD = "bear --append --output compile_commands.json --"

def make_linux(uml: bool = False, clean: bool = False):
    extra = " ARCH=um" if uml else ""

    # Clean up old build
    if clean:
        system(f"cd {LINUX_DIR} && make -j$(nproc) mrproper")

    # Generate config
    if not (LINUX_DIR / ".config").exists():
        system(f"cd {LINUX_DIR} && make -j$(nproc) defconfig {extra}")

    # Merge config
    system(
        f"cd {LINUX_DIR} && {LINUX_DIR}/scripts/kconfig/merge_config.sh {LINUX_DIR}/.config {LINUX_CUSTOM_CONFIG}"
    )

    # Build kernel
    system(f"cd {LINUX_DIR} && {BEAR_CMD} make -j$(nproc) {extra}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--uml", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--clean", action=argparse.BooleanOptionalAction, default=False)
    args = parser.parse_args()
    make_linux(**vars(args))
