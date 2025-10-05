#!/usr/bin/env python3

import argparse

from scripts import LINUX_DIR, PROJ_DIR, system

BEAR_CMD = "bear --append --output compile_commands.json --"

def make_linux(uml: bool = False, clean: bool = False):
    extra = " ARCH=um" if uml else ""

    # Clean up old build
    if clean:
        system(f"make -C {LINUX_DIR} -j$(nproc) mrproper")

    # Generate config
    if not (LINUX_DIR / ".config").exists():
        system(f"make -C {LINUX_DIR} -j$(nproc) allnoconfig {extra}")

    # Merge config
    system(
        f"cd {LINUX_DIR} && {LINUX_DIR}/scripts/kconfig/merge_config.sh {LINUX_DIR}/.config {PROJ_DIR}/config"
    )

    # Build kernel
    system(f"{BEAR_CMD} make -C {LINUX_DIR} -j$(nproc) {extra}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--uml", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--clean", action=argparse.BooleanOptionalAction, default=False)
    args = parser.parse_args()
    make_linux(**vars(args))
