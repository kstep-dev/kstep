#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
from typing import List, Optional

from scripts import PROJ_DIR, ROOTFS_IMG, Arch, get_linux_dir, get_log_path, system


def run_qemu(
    debug: bool = False,
    log_path: Optional[Path] = get_log_path(create=True),
    params: Optional[List[str]] = None,
):
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
        "isolcpus=nohz,managed_irq,1,2",
        "irqaffinity=0",
        "rcu_nocbs=1,2",
        "nohz_full=1,2",
        # "notsc",
        # "initcall_blacklist=spawn_ksoftirqd",
        # "noapictimer",
    ]

    if Arch.get() == Arch.X86_64:
        boot_args += ["console=ttyS0", "tsc=nowatchdog"]

    if params:
        # Everything after the `-` is passed to init
        # https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
        boot_args += ["-"]
        boot_args += params

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

    if log_path:
        cmd += [
            f"-chardev stdio,id=char0,mux=on,logfile={log_path},signal=off",
            "-serial chardev:char0",
            "-mon chardev=char0",
        ]

    if debug:
        cmd += ["-s", "-S"]

    system(" ".join(cmd))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--params", nargs="+", default=[])
    args = parser.parse_args()
    system(f"make -C {PROJ_DIR} -j$(nproc)")
    run_qemu(**vars(args))
