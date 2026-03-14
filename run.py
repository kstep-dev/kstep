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
    BUILD_DIR,
    LATEST_COV,
    LATEST_LOG,
    LATEST_OUTPUT,
    LINUX_CURR_DIR,
    LINUX_ROOT_DIR,
    LOGS_DIR,
    PROJ_DIR,
    QEMU_DIR,
    system,
    update_latest,
)
from scripts.corpus import GLOBAL_SIGNAL_CORPUS
from scripts.cov import cov_parse
from scripts.input_seq import input_seq_from_log

ARCH = os.uname().machine
assert ARCH in ("x86_64", "aarch64"), f"Unsupported architecture: {ARCH}"


@dataclass(frozen=True)
class Driver:
    name: str = "default"
    params: Iterable[str] = ()
    num_cpus: int = 2
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


def start_qemu(
    driver: Driver,
    linux_name: str,
    log_file: Optional[Path] = None,
    sock_file: Optional[Path] = None,
    debug: bool = False,
) -> subprocess.Popen:
    """Start QEMU in the background. Returns the process handle."""
    kvm_path = Path("/dev/kvm")
    if kvm_path.exists() and not os.access(kvm_path, os.R_OK):
        system(f"sudo chmod 666 {kvm_path}")

    qemu_path = get_qemu_path()
    kernel_img = BUILD_DIR / linux_name / "image"
    rootfs_img = BUILD_DIR / linux_name / "rootfs.cpio"

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = log_file or LOGS_DIR / f"log-{timestamp}.log"
    output_file = log_file.with_suffix(".jsonl")
    cov_file = log_file.with_suffix(".cov")
    update_latest(LATEST_LOG, log_file)
    update_latest(LATEST_OUTPUT, output_file)
    update_latest(LATEST_COV, cov_file)

    isol_cpus = f"1-{driver.num_cpus - 1}" if driver.num_cpus > 2 else "1"
    boot_args = [
        "rw",
        "nokaslr",
        "sched_verbose",
        f"isolcpus=nohz,managed_irq,{isol_cpus}",
        "irqaffinity=0",
        f"rcu_nocbs={isol_cpus}",
        f"nohz_full={isol_cpus}",
        "init=/init",
        "panic=-1",  # Exit immediately on panic
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
        f"-smp {driver.num_cpus}",
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
        # input and output files
        f"-chardev file,id=char1,path={output_file}",
        serial_device("char1"),
        # cov file
        f"-chardev file,id=char2,path={cov_file}",
        serial_device("char2"),
        # acceleration
        f"-accel {'kvm' if kvm_path.exists() else 'tcg'}",
    ]

    if sock_file is not None:
        cmd += [
            f"-chardev socket,id=char3,path={sock_file},server=on,wait=on",
            serial_device("char3"),
        ]

    if ARCH == "aarch64":
        cmd += ["-machine virt", "-cpu cortex-a57"]

    if debug:
        cmd += ["-s", "-S"]

    cmd_str = " ".join(cmd)
    logging.info(f"Starting QEMU: {cmd_str}")
    return subprocess.Popen(cmd_str, shell=True)

def run_qemu(
    driver: Driver,
    linux_name: str,
    log_file: Optional[Path] = None,
    sock_file: Optional[Path] = None,
    debug: bool = False,
):
    proc = start_qemu(driver, linux_name, log_file, sock_file, debug)
    proc.wait()

def print_run_results(
    linux_name: str,
    log_file=LATEST_LOG,
    output_file=LATEST_OUTPUT,
    cov_file=LATEST_COV,
):
    print(f"Log: {log_file}")
    print(f"Output: {output_file}")
    print(f"Cov: {cov_file}")

    # check if cov is empty and call analyze_new_signals and analyze_per_action_signals if it is not
    if cov_file.exists() and cov_file.stat().st_size != 0:
        # Parse the signal file and get the signal records and list
        signal_records = cov_parse(cov_file)

        # Symbolize the pcs
        GLOBAL_SIGNAL_CORPUS.update_pc_symbolize(
            signal_records=signal_records,
            linux_name=linux_name,
        )

        # Parse the input sequence from the log file
        seq = input_seq_from_log(log_file=log_file)

        print(signal_records.keys())

        # Analyze the new signals for the test
        new_signals = GLOBAL_SIGNAL_CORPUS.analyze_new_signals(
            seq=seq,
            signal_records=signal_records,
            linux_name=linux_name,
            output_path=LOGS_DIR / f"{cov_file}.new_edges.json",
        )

        # Analyze the per-action signals for the test if there are new signals
        if new_signals:
            GLOBAL_SIGNAL_CORPUS.analyze_per_action_signals(
                seq=seq,
                signal_records=signal_records,
                new_signals=new_signals,
                linux_name=linux_name,
                output_path=LOGS_DIR / f"{cov_file}.json",
            )


def is_port_free(port: int) -> bool:
    import socket

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("localhost", port)) != 0


def run_gdb(linux_name: str):
    import signal

    linux_dir = LINUX_ROOT_DIR / linux_name
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    args = [
        "-iex 'set pagination off'",
        "-iex 'set debuginfod enabled off'",
        f"-iex 'set auto-load safe-path {linux_dir}'",
        f"-ex 'source {linux_dir}/vmlinux-gdb.py'",
        "-ex 'target remote :1234'",
    ]
    system(f"gdb {linux_dir}/vmlinux " + " ".join(args))


def make_kstep(linux_name: str):
    system(f"make -C {PROJ_DIR} kstep LINUX_NAME={linux_name}")


def make_linux(linux_name: str):
    system(f"make -C {PROJ_DIR} linux LINUX_NAME={linux_name}")


def resolve_linux_name(linux_name: Optional[str] = None) -> str:
    if linux_name is not None:
        return linux_name
    name = LINUX_CURR_DIR.resolve().name
    logging.info(f"Using linux_name={name} (from linux/current)")
    return name


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_name", type=str, default=None)
    parser.add_argument("--log_file", type=Path, default=None)
    parser.add_argument("--debug", action="store_true")
    # See driver config
    parser.add_argument("name", type=str, default=None, nargs="?")
    parser.add_argument("--num_cpus", type=int, default=None)
    parser.add_argument("--mem_mb", type=int, default=None)
    parser.add_argument("--params", type=str, nargs="+", default=None)
    args = parser.parse_args()

    linux_name = resolve_linux_name(args.linux_name)

    if args.debug and not is_port_free(1234):
        logging.info("Port 1234 is already in use, running GDB...")
        run_gdb(linux_name=linux_name)
    else:
        make_kstep(linux_name=linux_name)
        driver = Driver(
            **{
                field.name: value
                for field in dataclasses.fields(Driver)
                if (value := getattr(args, field.name)) is not None
            }
        )
        run_qemu(
            driver=driver,
            linux_name=linux_name,
            debug=args.debug,
            log_file=args.log_file,
        )
        print_run_results(linux_name=linux_name)


if __name__ == "__main__":
    main()
