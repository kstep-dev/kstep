#!/usr/bin/env python3

import argparse

from scripts import (
    DATA_DIR,
    QEMU_DIR,
    decompress,
    download,
    system,
)

PACKAGES = [
    "build-essential",
    "flex",
    "bison",
    "bc",
    "libncurses-dev",
    "libssl-dev",
    "libelf-dev",
    "cpio",  # rootfs
    "bear",  # clangd completion
    "qemu-kvm",
    "gdb",
]


def apt_install(packages: list[str]):
    system("sudo rm -f /var/lib/man-db/auto-update")
    system("sudo apt update")
    system(f"sudo apt install -y {' '.join(packages)}")


def install_uv():
    system("curl -LsSf https://astral.sh/uv/install.sh | sh")


def qemu_install():
    url = "https://download.qemu.org/qemu-10.1.3.tar.xz"
    tarball_path = DATA_DIR / "qemu.tar.xz"
    src_dir = DATA_DIR / "qemu-10.1.3"
    build_dir = src_dir / "build"

    download(url, tarball_path)
    decompress(tarball_path, src_dir)

    opts = [
        "--target-list=x86_64-softmmu",
        "--static",
        "--without-default-features",
        "--enable-kvm",
        f"--prefix={QEMU_DIR}",
    ]
    system(f"mkdir -p {build_dir}")
    system(f"cd {build_dir} && ../configure {' '.join(opts)}")
    system(f"cd {build_dir} && make -j$(nproc)")
    system(f"cd {build_dir} && make install")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("type", choices=["ci-linux", "ci-qemu"], nargs="?")
    args = parser.parse_args()

    if args.type is None:
        apt_install(PACKAGES)
        install_uv()
    elif args.type == "ci-linux":
        apt_install(["libelf-dev"])
    elif args.type == "ci-qemu":
        apt_install(["libglib2.0-dev", "ninja-build"])
        qemu_install()
