#!/usr/bin/env python3

import argparse
import logging
from pathlib import Path

from scripts import (
    DATA_DIR,
    LINUX_CURR_DIR,
    LINUX_MASTER_DIR,
    LINUX_ROOT_DIR,
    PROJ_DIR,
    decompress,
    download,
    system,
)

LINUX_GIT_URL = "https://github.com/torvalds/linux.git"


def clone_master():
    if LINUX_MASTER_DIR.exists():
        logging.info(f"Linux master already cloned to {LINUX_MASTER_DIR}")
        return

    system(f"git clone {LINUX_GIT_URL} {LINUX_MASTER_DIR}")


def add_worktree(version: str, linux_dir: Path):
    if linux_dir.exists():
        logging.info(f"Linux {version} already cloned to {linux_dir}")
        return

    system(f"cd {LINUX_MASTER_DIR} && git fetch")
    system(f"cd {LINUX_MASTER_DIR} && git worktree prune -v")
    system(f"cd {LINUX_MASTER_DIR} && git worktree add {linux_dir} {version}")


def reset_git(linux_dir: Path):
    system(f"cd {linux_dir} && git restore .")


def set_current_linux(linux_dir: Path):
    LINUX_CURR_DIR.unlink(missing_ok=True)
    LINUX_CURR_DIR.symlink_to(linux_dir)
    logging.info(
        f"{LINUX_CURR_DIR.relative_to(PROJ_DIR)} now points to {linux_dir.relative_to(PROJ_DIR)}"
    )


def checkout_linux(version: str, linux_dir: Path, reset: bool, tarball: bool = False):
    if not tarball:
        clone_master()
        add_worktree(version, linux_dir)
        if reset:
            reset_git(linux_dir)
    else:
        tarball_path = DATA_DIR / f"{version}.tar.xz"
        version = version.removeprefix("v")
        major = version.split(".", 1)[0]
        url = (
            f"https://cdn.kernel.org/pub/linux/kernel/v{major}.x/linux-{version}.tar.xz"
        )
        download(url, tarball_path)
        decompress(tarball_path, linux_dir)
    set_current_linux(linux_dir)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("version", type=str, nargs="?", default="v6.14")
    parser.add_argument(
        "name",
        type=str,
        nargs="?",
        default=None,
        help="Name of the directory (default: <version>)",
    )
    parser.add_argument("--reset", action="store_true", default=False)
    parser.add_argument("--tarball", action="store_true", default=False)
    args = parser.parse_args()

    if args.name is None:
        args.name = args.version

    checkout_linux(
        version=args.version,
        linux_dir=LINUX_ROOT_DIR / args.name,
        reset=args.reset,
        tarball=args.tarball,
    )
