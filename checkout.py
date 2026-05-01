#!/usr/bin/env python3

import argparse
import logging
from dataclasses import dataclass
from pathlib import Path

from scripts import (
    BUILD_CURR_DIR,
    LINUX_MASTER_DIR,
    PROJ_DIR,
    build_dir,
    decompress,
    download,
    system,
)


@dataclass(frozen=True)
class Linux:
    # A descriptive name for the Linux version
    name: str
    # The version/commit of the kernel to use
    version: str
    # The patch to apply to the kernel
    patch: Path | None = None
    # Extra kernel config fragment to merge
    config: Path | None = None


def fmt_path(path: Path) -> str:
    return f"`{path.relative_to(PROJ_DIR)}`"


def clone_master():
    if LINUX_MASTER_DIR.exists():
        logging.info(f"Linux master already cloned to {fmt_path(LINUX_MASTER_DIR)}")
        return

    system(f"git clone https://github.com/torvalds/linux.git {LINUX_MASTER_DIR}")


def add_worktree(version: str, linux_dir: Path):
    system(f"cd {LINUX_MASTER_DIR} && git fetch")
    system(f"cd {LINUX_MASTER_DIR} && git worktree prune -v")
    system(f"cd {LINUX_MASTER_DIR} && git worktree add {linux_dir} {version}")


def set_current_build(name: str):
    target = build_dir(name)
    BUILD_CURR_DIR.parent.mkdir(parents=True, exist_ok=True)
    BUILD_CURR_DIR.unlink(missing_ok=True)
    BUILD_CURR_DIR.symlink_to(target)
    logging.info(f"{fmt_path(BUILD_CURR_DIR)} now points to {fmt_path(target)}")


def get_download_url(version: str) -> str:
    if "." in version:
        version = version.removeprefix("v")
        major = version.split(".", 1)[0]
        return (
            f"https://cdn.kernel.org/pub/linux/kernel/v{major}.x/linux-{version}.tar.xz"
        )
    else:
        return f"https://github.com/torvalds/linux/tarball/{version}"


def patch_linux(linux_dir: Path, patch: Path):
    system(f"cd {linux_dir} && patch -p1 --forward --batch < {patch}")


def checkout(
    version: str,
    name: str,
    patch: Path | None = None,
    tarball: bool = False,
):
    linux_dir = build_dir(name) / "linux"
    if linux_dir.exists():
        logging.info(f"{fmt_path(linux_dir)} already exists")
    else:
        linux_dir.parent.mkdir(parents=True, exist_ok=True)
        if tarball:
            tarball_path = build_dir(name) / f"{version}.tar.xz"
            url = get_download_url(version)
            download(url, tarball_path)
            decompress(tarball_path, linux_dir)
        else:
            clone_master()
            add_worktree(version, linux_dir)

        if patch:
            patch_linux(linux_dir, patch)
    set_current_build(name)


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
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--tar", dest="tarball", action="store_true", default=True,
                        help="Download via kernel.org/GitHub tarball (default)")
    source.add_argument("--git", dest="tarball", action="store_false",
                        help="Add a worktree from the master git clone")
    parser.add_argument("--patch", type=Path, default=None)
    args = parser.parse_args()

    checkout(
        version=args.version,
        name=args.name if args.name else args.version,
        tarball=args.tarball,
        patch=args.patch,
    )
