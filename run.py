#!/usr/bin/env python3

import argparse
import dataclasses
import logging
import os
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from make import ARCH, Build, build_kstep, build_linux
from scripts import (
    BUILD_CURR_DIR,
    BUILD_DIR,
    ResultDir,
    system,
)


@dataclass(frozen=True)
class Driver:
    name: str = "default"
    params: Iterable[str] = ()
    num_cpus: int = 2
    mem_mb: int = 512


def build_qemu_cmd(
    driver: Driver,
    kernel: str,
    result_dir: ResultDir,
    debug: bool = False,
    headless: bool = False,
    cpu_affinity: str | None = None,
    ram_file: Path | None = None,
) -> str:
    b = Build(kernel)
    kvm_path = Path("/dev/kvm")
    use_kvm = kvm_path.exists()
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
    ]

    if ARCH == "x86_64":
        boot_args += ["console=ttyS0", "tsc=nowatchdog", "tsc=reliable"]
        machine_opts = []
        tcg_cpu = "max"
        virtio_serial = "virtio-serial-pci"
    else:
        boot_args += ["console=ttyAMA0", "earlycon"]
        machine_opts = ["virt"]
        tcg_cpu = "cortex-a57"
        virtio_serial = "virtio-serial-device"

    # Everything after the `--` is passed to init
    # https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
    boot_args += ["--"]
    boot_args += [f"driver={driver.name}"]

    if driver.params:
        boot_args.extend(driver.params)

    # Guest RAM backed by a shared file: the host maps it and reads the driver's shared region
    # (kmod/shm.h) directly, the way the wasm build reads the emulator's memory. The fuzzer uses it.
    machine = []
    if ram_file is not None:
        machine_opts += ["memory-backend=ram"]
        machine += [f"-object memory-backend-file,id=ram,size={driver.mem_mb}M,mem-path={ram_file},share=on"]
    if machine_opts:
        machine.insert(0, f"-machine {','.join(machine_opts)}")

    cmd = [
        f"qemu-system-{ARCH}",
        *machine,
        f"-cpu {'host' if use_kvm else tcg_cpu}",
        f"-smp {driver.num_cpus}",
        f"-m {driver.mem_mb}M",
        f"-kernel {b.kernel}",
        f"-initrd {b.rootfs}",
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
        # kSTEP's channel: a single virtio console port, whose virtqueues the kmod drives itself
        # (kmod/io.c). One virtqueue kick per record instead of one port I/O exit per byte on a
        # 16550, which dominated per-command latency under emulation. The kernel console stays on
        # the UART: it is up from console_init, where the virtio transport is probed much later,
        # and it never drops lines.
        f"-device {virtio_serial},id=vs0",
        "-device virtconsole,bus=vs0.0,nr=0,chardev=char1",
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
    debug: bool = False,
    headless: bool = False,
):
    system(build_qemu_cmd(driver, kernel, result_dir, debug, headless))


def print_run_results(result_dir: ResultDir | None = None):
    if result_dir is None:
        result_dir = ResultDir("latest")
    print(f"Results saved to {result_dir}")


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
    parser.add_argument("--headless", action="store_true", help="no stdio console or monitor; the console goes to qemu.log only")
    parser.add_argument("--print_cmd", action="store_true", help="build kSTEP, print the QEMU command line and exit (the fuzzer runs it)")
    parser.add_argument("--ram_file", type=Path, default=None, help="back guest RAM with this shared file (the host reads kmod/shm.h from it)")
    parser.add_argument("--bug", type=str, default=None, help="a bugs.yaml entry: its driver unless a name is given, on its machine (CPUs, memory), on build/<bug>_buggy unless --build; explicit options override")
    # See driver config
    parser.add_argument("name", type=str, default=None, nargs="?", help="Driver name")
    parser.add_argument("--num_cpus", type=int, default=None)
    parser.add_argument("--mem_mb", type=int, default=None)
    parser.add_argument("--params", type=str, nargs="+", default=None)
    args = parser.parse_args()

    kernel = args.build or (f"{args.bug}_buggy" if args.bug else BUILD_CURR_DIR.resolve().name)

    if args.debug and not is_port_free(1234):
        logging.info("Port 1234 is already in use, running GDB...")
        run_gdb(kernel=kernel)
    else:
        b = Build(kernel)
        if args.rebuild_linux:
            build_linux(b)
        build_kstep(b)
        from reproduce import ALL_BUGS
        bug = ALL_BUGS[args.bug] if args.bug else None
        if bug and args.name is None:
            args.name = bug.driver.name
        driver = Driver(**(bug.machine if bug else {}) | {
            field.name: value
            for field in dataclasses.fields(Driver)
            if (value := getattr(args, field.name)) is not None
        })
        result_dir = ResultDir.create(args.label, set_latest=not args.print_cmd)
        if args.print_cmd:
            print(build_qemu_cmd(driver, kernel, result_dir, debug=args.debug, headless=True, ram_file=args.ram_file))
            return
        run_qemu(
            driver=driver,
            kernel=kernel,
            debug=args.debug,
            result_dir=result_dir,
            headless=args.headless,
        )
        print_run_results(result_dir=result_dir)


if __name__ == "__main__":
    main()
