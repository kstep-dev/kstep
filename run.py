#!/usr/bin/env python3

import argparse
import dataclasses
import logging
import subprocess
import os
import shutil
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, Optional

from scripts import (
    LINUX_BUILD_DIR,
    LINUX_CURR_DIR,
    LOG_LATEST,
    LOGS_DIR,
    OUT_LATEST,
    PROJ_DIR,
    QEMU_DIR,
    ROOTFS_DIR,
    Arch,
    system,
    system_with_pipe,
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

    path = QEMU_DIR / "install" / "bin" / name
    if path.exists():
        return path

    raise RuntimeError(f"QEMU executable not found: {name}")


def get_log_paths() -> tuple[Path, Path]:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = LOGS_DIR / f"log-{timestamp}.log"
    out_file = log_file.with_suffix(".out")

    log_file.touch()
    LOG_LATEST.unlink(missing_ok=True)
    LOG_LATEST.symlink_to(log_file)

    out_file.touch()
    OUT_LATEST.unlink(missing_ok=True)
    OUT_LATEST.symlink_to(out_file)

    return log_file, out_file


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
    linux_name = linux_dir.resolve().name
    kernel_img = LINUX_BUILD_DIR / linux_name
    rootfs_img = ROOTFS_DIR / f"{linux_name}.cpio"

    if not log_file:
        log_file, out_file = get_log_paths()
    else:
        out_file = log_file.with_suffix(".out")

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

    cmd = [
        str(qemu_path),
        f"-smp {driver.smp}",
        "-cpu max",
        f"-m {driver.mem_mb}M",
        f"-kernel {kernel_img}",
        f"-initrd {rootfs_img}",
        f'-append "{" ".join(boot_args)}"',
        "-nographic",
        # Prevent automatic reboot after panic
        "-no-reboot",
        # log file
        f"-chardev stdio,id=char0,mux=on,logfile={log_file},signal=off",
        "-serial chardev:char0",
        "-mon chardev=char0",
        # trace file
        f"-chardev file,id=char1,path={out_file}",
        "-serial chardev:char1",
        # acceleration
        f"-accel {'kvm' if kvm_path.exists() else 'tcg'}",
    ]

    if Arch.get() == Arch.ARM64:
        cmd += [
            "-machine virt",
            "-cpu cortex-a57",
        ]

    if debug:
        cmd += ["-s", "-S"]

    return system_with_pipe(" ".join(cmd))

def pipe_to_qemu(proc: subprocess.Popen, stdin_payload: str):
    if not proc.stdin:
        raise RuntimeError("QEMU stdin pipe is not available")
    proc.stdin.write(stdin_payload.encode())
    proc.stdin.flush()

def print_run_results(log_file: Optional[Path]=None):
    if not log_file:
        log_file, out_file = get_log_paths()
    else:
        out_file = log_file.with_suffix(".out")

    print("Log saved to", log_file)
    print("Output saved to", out_file)

    # Print the last line for status
    with out_file.open() as f:
        line = "Not found"
        for line in f:
            pass
        print(line.strip())


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
    parser.add_argument("--log_file", type=Path, default=None)
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
        proc = run_qemu(
            driver=driver,
            linux_dir=args.linux_dir,
            debug=args.debug,
            log_file=args.log_file,
        )

        return_code = proc.wait()
        print(f"Qemu returned with code: {return_code}")
        print_run_results(log_file=args.log_file)

if __name__ == "__main__":
    main()
