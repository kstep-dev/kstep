#!/usr/bin/env python3

import argparse
from checkout_linux import Linux, checkout_linux
from run import Driver, make_kstep, make_linux, pipe_to_qemu, print_run_results, run_qemu
from scripts import LINUX_ROOT_DIR, LOGS_DIR
from scripts import generate_input
import time

def smp_to_cpus(smp: str) -> int:
    try:
        return int(smp.split(",")[0])
    except (ValueError, AttributeError, IndexError):
        return 2

def run_test(
    linux: Linux,
    smp: str,
    steps: int,
    max_tasks: int,
    max_cgroups: int,
    seed: int,
):
    linux_dir = LINUX_ROOT_DIR / f"test_{linux.name}"
    checkout_linux(linux.version, linux_dir=linux_dir, tarball=True)
    make_linux(linux_dir=linux_dir)

    cpus = smp_to_cpus(smp)
    seq = generate_input(
        steps=steps,
        max_tasks=max_tasks,
        max_cgroups=max_cgroups,
        cpus=cpus,
        seed=seed,
    )

    driver = Driver(
        name="executor",
        params=(),
        smp=smp
    )

    make_kstep(linux_dir=linux_dir)

    log_file = LOGS_DIR / f"test_{linux.name}.log"

    proc = run_qemu(
        linux_dir=linux_dir,
        driver=driver,
        log_file=log_file,
    )

    time.sleep(1)

    for op in seq:
        pipe_to_qemu(proc=proc, stdin_payload=f"{op[0]},{op[1]},{op[2]},{op[3]}\n")

    pipe_to_qemu(proc=proc, stdin_payload="EXECUTE\n")
    pipe_to_qemu(proc=proc, stdin_payload="FINISH\n")

    return_code = proc.wait()
    print(f"QEMU returned with code: {return_code}")
    print_run_results(log_file=log_file)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_name", type=str, default="default")
    parser.add_argument("--linux_version", type=str, default="v6.14")
    parser.add_argument("--smp", type=str, default="10")
    parser.add_argument("--steps", type=int, default=80)
    parser.add_argument("--max_tasks", type=int, default=10)
    parser.add_argument("--max_cgroups", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    linux = Linux(
        name=args.linux_name,
        version=args.linux_version,
    )

    run_test(
        linux=linux,
        smp=args.smp,
        steps=args.steps,
        max_tasks=args.max_tasks,
        max_cgroups=args.max_cgroups,
        seed=args.seed,
    )

if __name__ == "__main__":
    main()
