#!/usr/bin/env python3

import argparse
import os
from enum import Enum
from pathlib import Path

from scripts import PROJ_DIR, ROOTFS_IMG, get_linux_dir, system


class Arch(Enum):
    X86_64 = "x86_64"
    ARM64 = "arm64"

    @classmethod
    def get(cls):
        machine = os.uname().machine
        if machine == "x86_64":
            return cls.X86_64
        elif machine == "aarch64":
            return cls.ARM64
        else:
            raise ValueError(f"Unsupported architecture: {machine}")


def run_qemu(debug: bool = False):
    kvm_path = Path("/dev/kvm")
    if kvm_path.exists() and not os.access(kvm_path, os.R_OK):
        system(f"sudo chmod 666 {kvm_path}")

    arch = Arch.get()
    exe = {
        Arch.X86_64: "qemu-system-x86_64",
        Arch.ARM64: "qemu-system-aarch64",
    }[arch]

    linux_dir = get_linux_dir()
    kernel_image_path = {
        Arch.X86_64: linux_dir / "arch/x86/boot/bzImage",
        Arch.ARM64: linux_dir / "arch/arm64/boot/Image",
    }[arch]

    boot_args = [
        "root=/dev/vda",
        "rw",
        "nokaslr",
        "sched_verbose",
        # "isolcpus=1,2",
        "rcu_nocbs=1,2",
        "nohz_full=1,2",
        "tsc=nowatchdog",
        # "notsc",
        # "initcall_blacklist=spawn_ksoftirqd",
        # "noapictimer",
    ]

    if Arch.get() == Arch.X86_64:
        boot_args.append("console=ttyS0")

    cmd = [
        exe,
        "-smp 3",
        "-cpu max",
        "-m 256M",
        f"-kernel {kernel_image_path}",
        f'-append "{" ".join(boot_args)}"',
        f"-drive if=virtio,file={ROOTFS_IMG},format=raw",
        "-nographic",
    ]
    if kvm_path.exists():
        cmd += ["-accel kvm"]
    else:
        cmd += ["-accel tcg"]
    if Arch.get() == Arch.ARM64:
        cmd += [
            "-machine virt",
            "-cpu cortex-a57",
        ]
    if debug:
        cmd += ["-s", "-S"]

    system(" ".join(cmd))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    system(f"make -C {PROJ_DIR} -j$(nproc)")
    run_qemu(**vars(args))
