#!/usr/bin/env python3

import argparse
import logging
from pathlib import Path

from scripts import (
    DOWNLOAD_DIR,
    LINUX_CURR_DIR,
    LINUX_MASTER_DIR,
    LINUX_ROOT_DIR,
    PROJ_DIR,
    decompress,
    download,
    system,
)


def fmt_path(path: Path) -> str:
    return f"`{path.relative_to(PROJ_DIR)}`"


def clone_master():
    if LINUX_MASTER_DIR.exists():
        logging.info(f"Linux master already cloned to {fmt_path(LINUX_MASTER_DIR)}")
        return

    system(f"git clone https://github.com/torvalds/linux.git {LINUX_MASTER_DIR}")


def add_worktree(version: str, linux_dir: Path):
    if linux_dir.exists():
        logging.info(f"{fmt_path(linux_dir)} already exists")
        return

    system(f"cd {LINUX_MASTER_DIR} && git fetch")
    system(f"cd {LINUX_MASTER_DIR} && git worktree prune -v")
    system(f"cd {LINUX_MASTER_DIR} && git worktree add {linux_dir} {version}")


def set_current_linux(linux_dir: Path):
    LINUX_CURR_DIR.unlink(missing_ok=True)
    LINUX_CURR_DIR.symlink_to(linux_dir)
    logging.info(f"{fmt_path(LINUX_CURR_DIR)} now points to {fmt_path(linux_dir)}")


def get_download_url(version: str) -> str:
    if "." in version:
        version = version.removeprefix("v")
        major = version.split(".", 1)[0]
        return (
            f"https://cdn.kernel.org/pub/linux/kernel/v{major}.x/linux-{version}.tar.xz"
        )
    else:
        return f"https://github.com/torvalds/linux/tarball/{version}"


def checkout_linux(version: str, linux_dir: Path, tarball: bool = False):
    if not tarball:
        clone_master()
        add_worktree(version, linux_dir)
    else:
        tarball_path = DOWNLOAD_DIR / f"{version}.tar.xz"
        url = get_download_url(version)
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
    parser.add_argument("--tarball", action="store_true", default=False)
    args = parser.parse_args()

    if args.name is None:
        args.name = args.version

    checkout_linux(
        version=args.version,
        linux_dir=LINUX_ROOT_DIR / args.name,
        tarball=args.tarball,
    )
