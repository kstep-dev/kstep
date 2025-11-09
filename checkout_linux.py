#!/usr/bin/env python3

import argparse
import logging
from pathlib import Path

from scripts import LINUX_CURR_DIR, LINUX_MASTER_DIR, get_linux_dir, system


def clone_master():
    if LINUX_MASTER_DIR.exists():
        logging.info(f"Linux master already cloned to {LINUX_MASTER_DIR}")
    else:
        system(f"git clone https://github.com/torvalds/linux.git {LINUX_MASTER_DIR}")


def add_worktree(version: str, linux_dir: Path):
    if linux_dir.exists():
        logging.info(f"Linux {version} already cloned to {linux_dir}")
    else:
        system(f"cd {LINUX_MASTER_DIR} && git worktree add {linux_dir} v{version}")


def set_current_linux(linux_dir: Path):
    """Set symlink for default version"""
    LINUX_CURR_DIR.unlink(missing_ok=True)
    LINUX_CURR_DIR.symlink_to(linux_dir)
    logging.info(f"Current Linux now points to {linux_dir}")


def checkout_linux(version: str, linux_dir: Path):
    clone_master()
    add_worktree(version, linux_dir)
    set_current_linux(linux_dir)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", type=str, default="6.14")
    args = parser.parse_args()

    checkout_linux(
        version=args.version,
        linux_dir=get_linux_dir(args.version),
    )
