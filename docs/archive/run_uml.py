#!/usr/bin/env python3

from scripts import LINUX_DIR, ROOTFS_IMG, make_rootfs, system


def run_uml():
    cmd = [
        f"{LINUX_DIR}/linux",
        f"ubd0={ROOTFS_IMG}",
        "root=/dev/ubda",
        "mem=256M",
        "rw",
    ]
    system(" ".join(cmd))


if __name__ == "__main__":
    make_rootfs(uml=True)
    run_uml()
