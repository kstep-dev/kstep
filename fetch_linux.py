#!/usr/bin/env python3

import argparse
import logging
import multiprocessing as mp
from pathlib import Path
from typing import Optional

from scripts import LINUX_ROOT_DIR, get_linux_dir, system

LINUX_MASTER_DIR = LINUX_ROOT_DIR / "master"


def clone_master():
    if LINUX_MASTER_DIR.exists():
        logging.info(f"Linux master already cloned to {LINUX_MASTER_DIR}")
    else:
        system(f"git clone https://github.com/torvalds/linux.git {LINUX_MASTER_DIR}")


def add_worktree(version: str, linux_dir: Optional[Path] = None):
    if linux_dir is None:
        linux_dir = get_linux_dir(version)
    if linux_dir.exists():
        logging.info(f"Linux {version} already cloned to {linux_dir}")
    else:
        system(f"cd {LINUX_MASTER_DIR} && git worktree add {linux_dir} v{version}")
    return linux_dir


def download_tarball(version: str):
    linux_dir = get_linux_dir(version)
    if linux_dir.exists():
        logging.info(f"Linux {version} source already exists in {linux_dir}")
        return linux_dir

    tarball_path = LINUX_ROOT_DIR / f"{version}.tar.xz"
    if tarball_path.exists():
        logging.info(f"Linux {version} tarball already downloaded to {tarball_path}")
    else:
        tarball_url = (
            f"https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-{version}.tar.xz"
        )
        system(f"wget {tarball_url} -O {tarball_path}")

    system(f"tar -xvf {tarball_path} -C {LINUX_ROOT_DIR}")
    return linux_dir


def set_current_linux(linux_dir: Path):
    """Set symlink for default version"""
    symlink = get_linux_dir()
    symlink.unlink(missing_ok=True)
    symlink.symlink_to(linux_dir)
    logging.info(f"Current Linux now points to {linux_dir}")


def fetch_linux(version: str, linux_dir: Path):
    clone_master()
    add_worktree(version, linux_dir=linux_dir)
    set_current_linux(linux_dir)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--versions", nargs="+", type=str, default=["6.14"])
    parser.add_argument("--tarball", help="Download tarball", action="store_true")
    args = parser.parse_args()

    if not args.tarball:
        clone_master()

    with mp.Pool(processes=mp.cpu_count()) as pool:
        if args.tarball:
            results = pool.map(download_tarball, args.versions)
        else:
            results = pool.map(add_worktree, args.versions)

    set_current_linux(results[0])
