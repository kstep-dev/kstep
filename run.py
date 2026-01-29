#!/usr/bin/env python3

import argparse
import dataclasses
import logging
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

from scripts import (
    LINUX_CURR_DIR,
    PROJ_DIR,
    QEMU_DIR,
    ROOTFS_DIR,
    Arch,
    create_log_path,
    system,
)


@dataclass(frozen=True)
class Driver:
    name: str = "default"
    params: Iterable[str] = ()
    smp: str = "2"
    mem_mb: int = 256


def get_qemu_path() -> Path:
    name = {
        Arch.X86_64: "qemu-system-x86_64",
        Arch.ARM64: "qemu-system-aarch64",
    }[Arch.get()]

    path = shutil.which(name)
    if path is not None:
        return Path(path)

    path = QEMU_DIR / "bin" / name
    if path.exists():
        return path

    raise RuntimeError(f"QEMU executable not found: {name}")


def get_kernel_image_path() -> Path:
    return {
        Arch.X86_64: Path("arch/x86/boot/bzImage"),
        Arch.ARM64: Path("arch/arm64/boot/Image"),
    }[Arch.get()]


def run_qemu(
    driver: Driver,
    linux_dir: Path,
    log_file: Optional[Path] = None,
    debug: bool = False,
):
    kvm_path = Path("/dev/kvm")
    if kvm_path.exists() and not os.access(kvm_path, os.R_OK):
        system(f"sudo chmod 666 {kvm_path}")

    qemu_path = get_qemu_path()
    kernel_image_path = linux_dir / get_kernel_image_path()

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
    boot_args += [f"driver={driver.name}"]

    if driver.params:
        boot_args.extend(driver.params)

    rootfs_img = ROOTFS_DIR / f"{linux_dir.name}.cpio"
    cmd = [
        str(qemu_path),
        f"-smp {driver.smp}",
        "-cpu max",
        f"-m {driver.mem_mb}M",
        f"-kernel {kernel_image_path}",
        f"-initrd {rootfs_img}",
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


def make_kstep(linux_dir: Path):
    system(f"make -C {PROJ_DIR} kstep LINUX_DIR={linux_dir}")


def make_linux(linux_dir: Path):
    system(f"make -C {PROJ_DIR} linux LINUX_DIR={linux_dir}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_dir", type=Path, default=LINUX_CURR_DIR)
    parser.add_argument("--log_file", type=Path, default=create_log_path())
    parser.add_argument("--debug", action="store_true")
    # See driver config
    parser.add_argument("name", type=str, default=None, nargs="?")
    parser.add_argument("--smp", type=str, default=None)
    parser.add_argument("--mem_mb", type=int, default=None)
    parser.add_argument("--params", type=str, nargs="+", default=None)
    args = parser.parse_args()

    if args.debug and not is_port_free(1234):
        logging.info("Port 1234 is already in use, running GDB...")
        run_gdb(linux_dir=args.linux_dir)
    else:
        make_kstep(linux_dir=args.linux_dir)
        driver = Driver(
            **{
                field.name: value
                for field in dataclasses.fields(Driver)
                if (value := getattr(args, field.name)) is not None
            }
        )
        run_qemu(
            driver=driver,
            linux_dir=args.linux_dir,
            debug=args.debug,
            log_file=args.log_file,
        )


if __name__ == "__main__":
    main()
