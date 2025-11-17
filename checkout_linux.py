#!/usr/bin/env python3

import argparse
import logging
from pathlib import Path

from scripts import LINUX_CURR_DIR, LINUX_MASTER_DIR, LINUX_ROOT_DIR, system


def clone_master():
    if LINUX_MASTER_DIR.exists():
        logging.info(f"Linux master already cloned to {LINUX_MASTER_DIR}")
    else:
        system(f"git clone https://github.com/torvalds/linux.git {LINUX_MASTER_DIR}")


def add_worktree(version: str, linux_dir: Path):
    if linux_dir.exists():
        logging.info(f"Linux {version} already cloned to {linux_dir}")
    else:
        system(f"cd {LINUX_MASTER_DIR} && git worktree add -f {linux_dir} {version}")

def reset_git(linux_dir: Path):
    system(f"cd {linux_dir} && git restore .")

def set_current_linux(linux_dir: Path):
    """Set symlink for default version"""
    LINUX_CURR_DIR.unlink(missing_ok=True)
    LINUX_CURR_DIR.symlink_to(linux_dir)
    logging.info(f"Current Linux now points to {linux_dir}")


def checkout_linux(version: str, linux_dir: Path, reset: bool):
    clone_master()
    add_worktree(version, linux_dir)
    if reset:
        reset_git(linux_dir)
    set_current_linux(linux_dir)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--version",
        type=str,
        default="v6.14",
        help="Linux branch/tag/commit to checkout",
    )
    parser.add_argument("--reset", action="store_true", default=False)
    args = parser.parse_args()

    checkout_linux(
        version=args.version,
        linux_dir=LINUX_ROOT_DIR / args.version,
        reset=args.reset,
    )
