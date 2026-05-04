#!/usr/bin/env python3

import argparse
import logging
from dataclasses import dataclass
from pathlib import Path

from scripts import (
    BUILD_CURR_DIR,
    BUILD_DIR,
    PROJ_DIR,
    decompress,
    download,
    system,
)

LINUX_MASTER_DIR = BUILD_DIR / "master"
LINUX_MASTER_URL = "https://github.com/gregkh/linux.git"


@dataclass(frozen=True)
class Linux:
    # A descriptive name for the Linux build
    name: str
    # Git ref (tag/branch/commit) of the kernel to use
    ref: str
    # The patch to apply to the kernel
    patch: Path | None = None
    # Extra kernel config fragment to merge
    config: Path | None = None


def fmt_path(path: Path) -> str:
    return f"`{path.relative_to(PROJ_DIR)}`"


def add_worktree(ref: str, linux_dir: Path):
    if not LINUX_MASTER_DIR.exists():
        system(f"git clone --filter=blob:none {LINUX_MASTER_URL} {LINUX_MASTER_DIR}")
    system(f"cd {LINUX_MASTER_DIR} && git fetch")
    system(f"cd {LINUX_MASTER_DIR} && git worktree prune -v")
    system(f"cd {LINUX_MASTER_DIR} && git worktree add {linux_dir} {ref}")


def set_current_build(kernel: str):
    BUILD_CURR_DIR.parent.mkdir(parents=True, exist_ok=True)
    BUILD_CURR_DIR.unlink(missing_ok=True)
    BUILD_CURR_DIR.symlink_to(BUILD_DIR / kernel)


def get_download_url(ref: str) -> str:
    if "." in ref:
        ref = ref.removeprefix("v")
        major = ref.split(".", 1)[0]
        return f"https://cdn.kernel.org/pub/linux/kernel/v{major}.x/linux-{ref}.tar.xz"
    else:
        return f"https://github.com/torvalds/linux/archive/{ref}.tar.gz"


def patch_linux(linux_dir: Path, patch: Path):
    system(f"cd {linux_dir} && patch -p1 --forward --batch < {patch}")


def checkout(
    ref: str,
    kernel: str,
    patch: Path | None = None,
    tarball: bool = False,
):
    linux_dir = BUILD_DIR / kernel / "linux"
    if not linux_dir.exists():
        linux_dir.parent.mkdir(parents=True, exist_ok=True)
        if tarball:
            tarball_path = BUILD_DIR / kernel / f"{ref}.tar.xz"
            url = get_download_url(ref)
            download(url, tarball_path)
            decompress(tarball_path, linux_dir)
        else:
            add_worktree(ref, linux_dir)

        if patch:
            patch_linux(linux_dir, patch)
    set_current_build(kernel)
    logging.info(f"Checked out Linux {ref} to {fmt_path(linux_dir)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("ref", type=str, nargs="?", default="v6.14")
    parser.add_argument("kernel", type=str, nargs="?", default=None)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--tar", dest="tarball", action="store_true", default=True)
    source.add_argument("--git", dest="tarball", action="store_false")
    parser.add_argument("--patch", type=Path, default=None)
    args = parser.parse_args()

    checkout(
        ref=args.ref,
        kernel=args.kernel if args.kernel else args.ref,
        tarball=args.tarball,
        patch=args.patch,
    )
