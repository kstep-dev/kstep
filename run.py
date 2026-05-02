#!/usr/bin/env python3

import argparse
import dataclasses
import logging
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

from scripts import (
    BUILD_CURR_DIR,
    ResultDir,
    get_build_dir,
    get_linux_dir,
    system,
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
    topology: Optional[str] = None
    frequency: Optional[str] = None
    capacity: Optional[str] = None


def build_qemu_cmd(
    driver: Driver,
    name: str,
    result_dir: ResultDir,
    use_sock: bool = False,
    debug: bool = False,
    headless: bool = False,
    cpu_affinity: Optional[str] = None,
) -> str:
    kvm_path = Path("/dev/kvm")
    if kvm_path.exists() and not os.access(kvm_path, os.R_OK):
        system(f"sudo chmod 666 {kvm_path}")

    build = get_build_dir(name)
    kernel_img = build / "kernel"
    rootfs_img = build / "rootfs.cpio"

    isol_cpus = f"1-{driver.num_cpus - 1}" if driver.num_cpus > 2 else "1"
    boot_args = [
        "rw",
        "nokaslr",
        "loglevel=7",  # print up to KERN_DEBUG; ensures KERN_WARNING (4) always appears
        "sched_verbose",
        f"isolcpus=nohz,managed_irq,{isol_cpus}",
        "irqaffinity=0",
        f"rcu_nocbs={isol_cpus}",
        f"nohz_full={isol_cpus}",
        "init=/user",
        "panic=-1",  # Exit immediately on panic
        "console=ttyS0",
    ]

    if ARCH == "x86_64":
        boot_args += ["tsc=nowatchdog"]

    # Everything after the `--` is passed to init
    # https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
    boot_args += ["--"]
    boot_args += [f"driver={driver.name}"]
    if driver.topology:
        boot_args += [f"topology={driver.topology}"]
    if driver.frequency:
        boot_args += [f"frequency={driver.frequency}"]
    if driver.capacity:
        boot_args += [f"capacity={driver.capacity}"]

    if driver.params:
        boot_args.extend(driver.params)

    def serial_device(name: str):
        if ARCH == "aarch64":
            return f"-device pci-serial,chardev={name}"
        return f"-serial chardev:{name}"

    cmd = [
        f"qemu-system-{ARCH}",
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
        serial_device("char0"),
        # input and output files
        f"-chardev file,id=char1,path={result_dir.output}",
        serial_device("char1"),
        # cov file
        f"-chardev file,id=char2,path={result_dir.cov}",
        serial_device("char2"),
        # acceleration
        f"-accel {'kvm' if kvm_path.exists() else 'tcg'}",
    ]

    
    if headless:
        cmd += [f"-chardev file,id=char0,path={result_dir.log}"]
    else:
        cmd += [
            f"-chardev stdio,id=char0,mux=on,logfile={result_dir.log},signal=off",
            "-mon chardev=char0",
        ]

    if use_sock:
        cmd += [
            f"-chardev socket,id=char3,path={result_dir.sock},server=on,wait=on",
            serial_device("char3"),
        ]

    if ARCH == "aarch64":
        cmd += ["-machine virt", "-cpu cortex-a57"]

    if debug:
        cmd += ["-s", "-S"]

    cmd_str = " ".join(cmd)
    if cpu_affinity is not None:
        cmd_str = f"taskset -c {cpu_affinity} {cmd_str}"
    return cmd_str


def run_qemu(
    driver: Driver,
    name: str,
    result_dir: ResultDir,
    use_sock: bool = False,
    debug: bool = False,
    headless: bool = False,
):
    system(build_qemu_cmd(driver, name, result_dir, use_sock, debug, headless))


def print_run_results(name: str, result_dir: Optional[ResultDir] = None):
    if result_dir is None:
        result_dir = ResultDir("latest")
    print(f"Results saved to {result_dir}")

    # check if cov is empty and call analyze_new_signals and analyze_per_action_signals if it is not
    if result_dir.cov.exists() and result_dir.cov.stat().st_size != 0:
        # Parse the signal file and get the signal records and list
        signal_records = cov_parse(result_dir.cov)

        # Symbolize the pcs
        GLOBAL_SIGNAL_CORPUS.update_pc_symbolize(
            signal_records=signal_records,
            name=name,
        )

        # Parse the input sequence from the log file
        seq = input_seq_from_log(log_file=result_dir.log)

        print(signal_records.keys())

        # Analyze the new signals for the test
        new_signal_info = GLOBAL_SIGNAL_CORPUS.analyze_new_signals(
            seq=seq,
            signal_records=signal_records,
            name=name,
            output_path=result_dir.path / "kstep.cov.new_edges.json",
        )

        # Analyze the per-action signals for the test if there are new signals
        if new_signal_info:
            new_signals, _ = new_signal_info
            GLOBAL_SIGNAL_CORPUS.analyze_per_action_signals(
                seq=seq,
                signal_records=signal_records,
                new_signals=new_signals,
                name=name,
                output_path=result_dir.path / "kstep.cov.json",
            )


def is_port_free(port: int) -> bool:
    import socket

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("localhost", port)) != 0


def run_gdb(name: str):
    import signal

    linux_dir = get_linux_dir(name)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    args = [
        "-iex 'set pagination off'",
        "-iex 'set debuginfod enabled off'",
        f"-iex 'set auto-load safe-path {linux_dir}'",
        f"-ex 'source {linux_dir}/vmlinux-gdb.py'",
        "-ex 'target remote :1234'",
    ]
    system(f"gdb {linux_dir}/vmlinux " + " ".join(args))


def make_kstep(name: str, log: bool = False):
    cmd = f"make kstep NAME={name}"
    if log:
        cmd += f" >> {get_build_dir(name) / 'build.log'}"
    system(cmd)


def make_linux(name: str, config: Optional[Path] = None, log: bool = False):
    cmd = f"make linux NAME={name}"
    if config:
        cmd += f" KSTEP_EXTRA_CONFIG={config}"
    if log:
        cmd += f" >> {get_build_dir(name) / 'build.log'}"
    system(cmd)


def resolve_name(name: Optional[str] = None) -> str:
    if name is not None:
        return name
    name = BUILD_CURR_DIR.resolve().name
    logging.info(f"Using name={name} (from build/current)")
    return name


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", type=str, default=None)
    parser.add_argument("--result_name", type=str, default=None)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--rebuild_linux", action="store_true", default=False)
    # See driver config
    parser.add_argument("name", type=str, default=None, nargs="?")
    parser.add_argument("--num_cpus", type=int, default=None)
    parser.add_argument("--mem_mb", type=int, default=None)
    parser.add_argument("--topology", type=str, default=None)
    parser.add_argument("--frequency", type=str, default=None)
    parser.add_argument("--capacity", type=str, default=None)
    parser.add_argument("--params", type=str, nargs="+", default=None)
    args = parser.parse_args()

    name = resolve_name(args.name)

    if args.debug and not is_port_free(1234):
        logging.info("Port 1234 is already in use, running GDB...")
        run_gdb(name=name)
    else:
        if args.rebuild_linux:
            make_linux(name=name)
        make_kstep(name=name)
        driver = Driver(**{
            field.name: value
            for field in dataclasses.fields(Driver)
            if (value := getattr(args, field.name)) is not None
        })
        result_dir = ResultDir.create(args.result_name)
        run_qemu(
            driver=driver,
            name=name,
            debug=args.debug,
            result_dir=result_dir,
        )
        print_run_results(name=name, result_dir=result_dir)


if __name__ == "__main__":
    main()
