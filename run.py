#!/usr/bin/env python3

import argparse
import logging
import os
import shutil
from pathlib import Path
from typing import Iterable, Optional

from scripts import (
    LINUX_CURR_DIR,
    PROJ_DIR,
    QEMU_DIR,
    ROOTFS_IMG,
    Arch,
    create_log_path,
    system,
)


def make_kstep():
    system(f"make -C {PROJ_DIR} kstep")


def make_linux():
    system(f"make -C {PROJ_DIR} linux")


def get_qemu_path() -> Path:
    arch = Arch.get()
    name = {
        Arch.X86_64: "qemu-system-x86_64",
        Arch.ARM64: "qemu-system-aarch64",
    }[arch]

    path = shutil.which(name)
    if path is not None:
        return Path(path)

    path = QEMU_DIR / "bin" / name
    if path.exists():
        return path

    raise RuntimeError(f"QEMU executable not found: {name}")


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

    qemu_path = get_qemu_path()

    arch = Arch.get()
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
        "init=/init",
        "panic=-1",  # Exit immediately on panic
        "quiet",  # Disable printk to be enabled later
        "printk.time=0",  # Disable printk time to be enabled later
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
        str(qemu_path),
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


def is_port_free(port: int) -> bool:
    import socket

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("localhost", port)) != 0


def run_gdb(linux_dir: Path):
    import signal

    signal.signal(signal.SIGINT, signal.SIG_IGN)
    args = [
        "-iex 'set pagination off'",
        "-iex 'set debuginfod enabled off'",
        f"-iex 'set auto-load safe-path {linux_dir}'",
        f"-ex 'source {linux_dir}/vmlinux-gdb.py'",
        "-ex 'target remote :1234'",
    ]
    system(f"gdb {linux_dir}/vmlinux " + " ".join(args))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_dir", type=Path, default=LINUX_CURR_DIR)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--driver", type=str, default=None)
    parser.add_argument("--log_file", type=Path, default=create_log_path())
    parser.add_argument("--smp", type=str, default="3")
    parser.add_argument("--params", type=str, nargs="+")
    args = parser.parse_args()

    if args.debug and not is_port_free(1234):
        logging.info("Port 1234 is already in use, running GDB...")
        run_gdb(args.linux_dir)
    else:
        make_kstep()
        run_qemu(**vars(args))
