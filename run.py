#!/usr/bin/env python3

import argparse
import dataclasses
import logging
import os
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from make import HOST_ARCH, Build, build_kstep, build_linux
from scripts import (
    BUILD_CURR_DIR,
    BUILD_DIR,
    ResultDir,
    system,
)
from scripts.corpus import GLOBAL_SIGNAL_CORPUS
from scripts.cov import cov_parse
from scripts.input_seq import input_seq_from_log


@dataclass(frozen=True)
class Driver:
    name: str = "default"
    params: Iterable[str] = ()
    num_cpus: int = 2
    mem_mb: int = 512
    topology: str | None = None
    frequency: str | None = None
    capacity: str | None = None


def build_qemu_cmd(
    driver: Driver,
    kernel: str,
    result_dir: ResultDir,
    use_sock: bool = False,
    debug: bool = False,
    headless: bool = False,
    cpu_affinity: str | None = None,
) -> str:
    b = Build(kernel)
    ARCH = b.arch
    kernel_img = b.dir / "kernel"
    rootfs_img = b.rootfs

    kvm_path = Path("/dev/kvm")
    use_kvm = ARCH == HOST_ARCH and kvm_path.exists()
    if use_kvm and not os.access(kvm_path, os.R_OK):
        system(f"sudo chmod 666 {kvm_path}")

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
        # the machine's UART: a 16550 on the pc machine, the PL011 on virt (with an earlycon)
        "console=ttyS0" if ARCH == "x86_64" else "console=ttyAMA0 earlycon",
    ]

    if ARCH == "x86_64":
        boot_args += ["tsc=nowatchdog", "tsc=reliable"]

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

    cmd = [
        b.arch.qemu,
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
        # log file: the kernel console on the machine's UART
        "-serial chardev:char0",
        # structured JSON: logged to the jsonl file, and a socket so a client can also talk
        # to the driver (the cli driver reads its commands here)
        f"-chardev socket,id=char1,path={result_dir.output}.sock,server=on,wait=off,logfile={result_dir.output}",
        # kSTEP's channels are virtio console ports (/dev/hvc0..2): one virtqueue kick per write
        # instead of one port I/O exit per byte on a 16550, which dominated per-command latency
        # under emulation. The kernel console stays on ttyS0: it is up from console_init, while
        # hvc0 only exists once PCI has been enumerated, and it never drops lines.
        "-device virtio-serial-pci,id=vs0",
        "-device virtconsole,bus=vs0.0,nr=0,chardev=char1",
        # cov file
        f"-chardev file,id=char2,path={result_dir.cov}",
        "-device virtconsole,bus=vs0.0,nr=1,chardev=char2",
        # acceleration
        f"-accel {'kvm' if use_kvm else 'tcg'}",
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
            "-device virtconsole,bus=vs0.0,nr=2,chardev=char3",
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
    kernel: str,
    result_dir: ResultDir,
    use_sock: bool = False,
    debug: bool = False,
    headless: bool = False,
):
    system(build_qemu_cmd(driver, kernel, result_dir, use_sock, debug, headless))


def print_run_results(kernel: str, result_dir: ResultDir | None = None):
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
            kernel=kernel,
        )

        # Parse the input sequence from the log file
        seq = input_seq_from_log(log_file=result_dir.log)

        print(signal_records.keys())

        # Analyze the new signals for the test
        new_signal_info = GLOBAL_SIGNAL_CORPUS.analyze_new_signals(
            seq=seq,
            signal_records=signal_records,
            kernel=kernel,
            output_path=result_dir.path / "kstep.cov.new_edges.json",
        )

        # Analyze the per-action signals for the test if there are new signals
        if new_signal_info:
            new_signals, _ = new_signal_info
            GLOBAL_SIGNAL_CORPUS.analyze_per_action_signals(
                seq=seq,
                signal_records=signal_records,
                new_signals=new_signals,
                kernel=kernel,
                output_path=result_dir.path / "kstep.cov.json",
            )


def is_port_free(port: int) -> bool:
    import socket

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        return s.connect_ex(("localhost", port)) != 0


def run_gdb(kernel: str):
    import signal

    linux_dir = BUILD_DIR / kernel / "linux"
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    args = [
        "-iex 'set pagination off'",
        "-iex 'set debuginfod enabled off'",
        f"-iex 'set auto-load safe-path {linux_dir}'",
        f"-ex 'source {linux_dir}/vmlinux-gdb.py'",
        "-ex 'target remote :1234'",
    ]
    system(f"gdb {linux_dir}/vmlinux " + " ".join(args))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=str, default=None, help="build name under build/ (default: build/current)")
    parser.add_argument("--label", type=str, default=None)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--rebuild_linux", action="store_true", default=False)
    # See driver config
    parser.add_argument("name", type=str, default=None, nargs="?", help="Driver name")
    parser.add_argument("--num_cpus", type=int, default=None)
    parser.add_argument("--mem_mb", type=int, default=None)
    parser.add_argument("--topology", type=str, default=None)
    parser.add_argument("--frequency", type=str, default=None)
    parser.add_argument("--capacity", type=str, default=None)
    parser.add_argument("--params", type=str, nargs="+", default=None)
    args = parser.parse_args()

    kernel = args.build or BUILD_CURR_DIR.resolve().name

    if args.debug and not is_port_free(1234):
        logging.info("Port 1234 is already in use, running GDB...")
        run_gdb(kernel=kernel)
    else:
        b = Build(kernel)
        if args.rebuild_linux:
            build_linux(b)
        build_kstep(b)
        driver = Driver(**{
            field.name: value
            for field in dataclasses.fields(Driver)
            if (value := getattr(args, field.name)) is not None
        })
        result_dir = ResultDir.create(args.label)
        run_qemu(
            driver=driver,
            kernel=kernel,
            debug=args.debug,
            result_dir=result_dir,
        )
        print_run_results(kernel=kernel, result_dir=result_dir)


if __name__ == "__main__":
    main()
