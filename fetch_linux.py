#!/usr/bin/env python3

import argparse
import logging

from scripts import LINUX_DIR, LINUX_VERSIONS_DIR, system

LINUX_GIT_URL = "https://github.com/torvalds/linux.git"
LINUX_MASTER_DIR = LINUX_VERSIONS_DIR / "master"


def clone_master():
    if LINUX_MASTER_DIR.exists():
        logging.info(f"Linux master already cloned to {LINUX_MASTER_DIR}")
    else:
        system(f"git clone {LINUX_GIT_URL} {LINUX_MASTER_DIR}")


def add_worktree(version: str):
    worktree_dir = LINUX_VERSIONS_DIR / f"linux-{version}-git"
    if worktree_dir.exists():
        logging.info(f"Linux {version} already cloned to {worktree_dir}")
    else:
        system(f"cd {LINUX_MASTER_DIR} && git worktree add {worktree_dir} {version}")
    return worktree_dir


def download_tarball(version: str):
    tarball_url = f"https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-{version}.tar.xz"
    tarball_path = LINUX_VERSIONS_DIR / f"{version}.tar.xz"
    if tarball_path.exists():
        logging.info(f"Linux {version} tarball already downloaded to {tarball_path}")
    else:
        system(f"wget {tarball_url} -O {tarball_path}")

    src_path = LINUX_VERSIONS_DIR / f"linux-{version}"
    if src_path.exists():
        logging.info(f"Linux {version} source already extracted to {src_path}")
    else:
        system(f"tar -xvf {tarball_path} -C {LINUX_VERSIONS_DIR}")
    return src_path


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--versions", nargs="+", type=str, default=["6.14"])
    parser.add_argument("--tarball", help="Download tarball", action="store_true")
    args = parser.parse_args()

    if not args.tarball:
        clone_master()

    for version in args.versions:
        if args.tarball:
            path = download_tarball(version)
        else:
            path = add_worktree(version)

        if version == args.versions[0]:
            LINUX_DIR.unlink(missing_ok=True)
            LINUX_DIR.symlink_to(path)
