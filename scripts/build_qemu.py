#!/usr/bin/env python3

import argparse
from pathlib import Path

from consts import DOWNLOAD_DIR, QEMU_DIR
from utils import decompress, download, system


def build_qemu(
    version: str,
    src_dir: Path = QEMU_DIR / "src",
    build_dir: Path = QEMU_DIR / "build",
    install_dir: Path = QEMU_DIR / "install",
):
    url = f"https://download.qemu.org/qemu-{version}.tar.xz"
    download_path = DOWNLOAD_DIR / f"qemu-{version}.tar.xz"
    download(url, download_path)
    decompress(download_path, src_dir)

    opts = [
        "--target-list=x86_64-softmmu",
        "--without-default-features",
        "--enable-kvm",
        f"--prefix={install_dir}",
    ]
    system(f"mkdir -p {build_dir}")
    system(f"cd {build_dir} && {src_dir}/configure {' '.join(opts)}")
    system(f"cd {build_dir} && make -j$(nproc)")
    system(f"cd {build_dir} && make install")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("version", type=str, default="10.2.0", nargs="?")
    args = parser.parse_args()
    build_qemu(version=args.version)
