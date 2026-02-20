#!/usr/bin/env python3

import argparse
import dataclasses
import logging
import os
import shutil
import subprocess
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, Optional

from scripts import (
    LATEST_COV,
    LATEST_LOG,
    LATEST_OUT,
    LATEST_SIGNAL,
    LINUX_BUILD_DIR,
    LINUX_CURR_DIR,
    LOGS_DIR,
    PROJ_DIR,
    QEMU_DIR,
    ROOTFS_DIR,
    kcov_symbolize,
    parse_signal_file,
    system,
    system_with_pipe,
    update_latest,
)

ARCH = os.uname().machine
assert ARCH in ("x86_64", "aarch64"), f"Unsupported architecture: {ARCH}"


@dataclass(frozen=True)
class Driver:
    name: str = "default"
    params: Iterable[str] = ()
    smp: str = "2"
    mem_mb: int = 512


def get_qemu_path() -> Path:
    name = f"qemu-system-{ARCH}"

    path = shutil.which(name)
    if path is not None:
        return Path(path)

    path = QEMU_DIR / "install" / "bin" / name
    if path.exists():
        return path

    raise RuntimeError(f"QEMU executable not found: {name}")


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

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = log_file or LOGS_DIR / f"log-{timestamp}.log"
    out_file = log_file.with_suffix(".out")
    cov_file = log_file.with_suffix(".cov")
    signal_file = log_file.with_suffix(".signal")
    update_latest(LATEST_LOG, log_file)
    update_latest(LATEST_OUT, out_file)
    update_latest(LATEST_COV, cov_file)
    update_latest(LATEST_SIGNAL, signal_file)

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
        "console=ttyS0",
    ]

    if ARCH == "x86_64":
        boot_args += ["tsc=nowatchdog"]

    # Everything after the `--` is passed to init
    # https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
    boot_args += ["--"]
    boot_args += [f"driver={driver.name}"]

    if driver.params:
        boot_args.extend(driver.params)

    def serial_device(name: str):
        if ARCH == "aarch64":
            return f"-device pci-serial,chardev={name}"
        return f"-serial chardev:{name}"

    cmd = [
        str(qemu_path),
        f"-smp {driver.smp}",
        "-cpu max",
        f"-m {driver.mem_mb}M",
        f"-kernel {kernel_img}",
        f"-initrd {rootfs_img}",
        f'-append "{" ".join(boot_args)}"',
        "-nographic",
        "-nodefaults",
        # Prevent automatic reboot after panic
        "-no-reboot",
        # log file
        f"-chardev stdio,id=char0,mux=on,logfile={log_file},signal=off",
        "-mon chardev=char0",
        serial_device("char0"),
        # out file
        f"-chardev file,id=char1,path={out_file}",
        serial_device("char1"),
        # cov file
        f"-chardev file,id=char2,path={cov_file}",
        serial_device("char2"),
        # signal file
        f"-chardev file,id=char3,path={signal_file}",
        serial_device("char3"),
        # acceleration
        f"-accel {'kvm' if kvm_path.exists() else 'tcg'}",
    ]

    if ARCH == "aarch64":
        cmd += ["-machine virt", "-cpu cortex-a57"]

    if debug:
        cmd += ["-s", "-S"]

    return system_with_pipe(" ".join(cmd))


def pipe_to_qemu(proc: subprocess.Popen, stdin_payload: str):
    if not proc.stdin:
        raise RuntimeError("QEMU stdin pipe is not available")
    proc.stdin.write(stdin_payload.encode())
    proc.stdin.flush()


def print_run_results(
    log_file=LATEST_LOG, 
    out_file=LATEST_OUT, 
    cov_file=LATEST_COV, 
    signal_file=LATEST_SIGNAL,
    vmlinux: Optional[Path] = None,
):
    print(f"Log: {log_file}")
    print(f"Out: {out_file}")
    print(f"Cov: {cov_file}")
    print(f"Signal: {signal_file}")

    # Print the last line for status
    with out_file.open() as f:
        line = "Not found"
        for line in f:
            pass
        print(line.strip())

    # check if cov is empty and call kcov_symbolize if it is not
    if cov_file.exists() and cov_file.stat().st_size != 0:
        linux_name = LINUX_CURR_DIR.resolve().name
        vmlinux = LINUX_BUILD_DIR / f"{linux_name}.vmlinux"
        kcov_symbolize(cov_file=cov_file, vmlinux=vmlinux)

    # check if signal (edge coverage) is empty and parse it if it is not
    if signal_file.exists() and signal_file.stat().st_size != 0:
        parse_signal_file(signal_file)


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
        print_run_results()


if __name__ == "__main__":
    main()
