#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
from typing import Iterable, Optional

from scripts import LINUX_CURR_DIR, PROJ_DIR, ROOTFS_IMG, Arch, get_log_path, system


def make_kstep():
    system(f"make -C {PROJ_DIR} -j$(nproc)")


def run_qemu(
    linux_dir: Path,
    debug: bool = False,
    log_file: Optional[Path] = None,
    driver: Optional[str] = None,
    params: Iterable[str] = (),
    smp: str = "3",
    mem_mb: int = 256,
):
    kvm_path = Path("/dev/kvm")
    if kvm_path.exists() and not os.access(kvm_path, os.R_OK):
        system(f"sudo chmod 666 {kvm_path}")

    arch = Arch.get()
    exe = {
        Arch.X86_64: "qemu-system-x86_64",
        Arch.ARM64: "qemu-system-aarch64",
    }[arch]

    kernel_image_path = {
        Arch.X86_64: linux_dir / "arch/x86/boot/bzImage",
        Arch.ARM64: linux_dir / "arch/arm64/boot/Image",
    }[arch]

    boot_args = [
        "rw",
        "nokaslr",
        "sched_verbose",
        "isolcpus=nohz,managed_irq,1,2",
        "irqaffinity=0",
        "rcu_nocbs=1,2",
        "nohz_full=1,2",
        "panic=-1",  # Exit immediately on panic
        "init=/init",
        "printk.time=0",  # Disable printk time
    ]

    if Arch.get() == Arch.X86_64:
        boot_args += ["console=ttyS0", "tsc=nowatchdog"]

    # Everything after the `--` is passed to init
    # https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
    boot_args += ["--"]
    if driver:
        boot_args += [f"driver={driver}"]

    if params:
        boot_args.extend(params)

    cmd = [
        exe,
        f"-smp {smp}",
        "-cpu max",
        f"-m {mem_mb}M",
        f"-kernel {kernel_image_path}",
        f"-initrd {ROOTFS_IMG}",
        f'-append "{" ".join(boot_args)}"',
        "-nographic",
        "-no-reboot",  # Prevent automatic reboot after panic
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

    if log_file:
        cmd += [
            f"-chardev stdio,id=char0,mux=on,logfile={log_file},signal=off",
            "-serial chardev:char0",
            "-mon chardev=char0",
        ]

    if debug:
        cmd += ["-s", "-S"]

    system(" ".join(cmd))

    if log_file:
        with log_file.open() as f:
            if any("Kernel panic" in line for line in f):
                raise RuntimeError("Kernel exited with panic")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_dir", type=Path, default=LINUX_CURR_DIR)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--driver", type=str, default=None)
    parser.add_argument("--log_file", type=Path, default=get_log_path(create=True))
    parser.add_argument("--smp", type=str, default="3")
    parser.add_argument("--params", type=str, nargs="+")
    args = parser.parse_args()
    make_kstep()
    run_qemu(**vars(args))
