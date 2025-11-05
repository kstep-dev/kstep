#!/usr/bin/env python3

import argparse
from pathlib import Path

from fetch_linux import fetch_linux
from make_linux import make_linux
from run_qemu import run_qemu
from scripts import LOGS_DIR, PROJ_DIR, get_linux_dir, system

versions_map = {
    "aa3ee4f": "6.14",
    "cd9626e": "6.12-rc3",
    "bbce3de": "6.14"
}


def patch_linux(linux_dir: Path, patch_file: Path):
    system(f"cd {linux_dir} && git apply {patch_file} && cd -")


def reset_git(linux_dir: Path):
    system(f"cd {linux_dir} && git restore . && cd -")


def main(version: str, controller: str, clean: bool = False):
    linux_dir = get_linux_dir(version)

    # Clean up old kmod build as it may conflicts with the new kernel build
    if clean:
        system(f"make -C {PROJ_DIR} clean")

    fetch_linux(version, linux_dir=linux_dir)
    reset_git(linux_dir)

    # patched initial min_vruntime
    patch_file = f"{PROJ_DIR}/linux/{version}-vruntime_min_init.patch"
    patch_linux(linux_dir, patch_file)

    # Run the buggy version
    make_linux(linux_dir)
    run_qemu(
        linux_dir=linux_dir,
        params=[f"controller={controller}"],
        log_file=LOGS_DIR / f"{controller}_buggy.log",
    )

    # Run the fixed version
    patch_file = f"{PROJ_DIR}/linux/{version}-{controller}_fix.patch"
    patch_linux(linux_dir, patch_file)

    make_linux(linux_dir)
    run_qemu(
        linux_dir=linux_dir,
        params=[f"controller={controller}"],
        log_file=LOGS_DIR / f"{controller}_fixed.log",
    )

    # plot the logs
    system(f"./plot/plot_cur_task.py --controller={controller}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="aa3ee4f")
    parser.add_argument("--clean", action="store_true", default=False)

    args = parser.parse_args()

    version = versions_map.get(args.controller)
    if not version:
        parser.error(
            f"controller '{args.controller}' not found in versions_map. Valid options are: {list(versions_map.keys())}"
        )
    main(version=version, controller=args.controller, clean=args.clean)
