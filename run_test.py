#!/usr/bin/env python3

import argparse
import time

from checkout_linux import Linux, checkout_linux
from run import (
    Driver,
    make_kstep,
    make_linux,
    pipe_to_qemu,
    print_run_results,
    run_qemu,
)
from scripts import LINUX_ROOT_DIR, LOGS_DIR, generate_input

def smp_to_cpus(smp: str) -> int:
    try:
        return int(smp.split(",")[0])
    except (ValueError, AttributeError, IndexError):
        return 2

def assert_exec_matches_generated(log_file, seq):
    i = 0
    f = open(log_file, "r")
    for line in f:
        exec_pos = line.find("EXECOP")
        if exec_pos == -1:
            continue
        line = line[exec_pos + len("EXECOP") :].strip()
        parts = line.split()
        if len(parts) != 4 or tuple(int(v) for v in parts[:4]) != seq[i]:
            raise AssertionError(f"EXECOP sequence mismatch. Expected {seq[i]}, got {parts[:4]}")
        i += 1
    f.close()
    if i != len(seq):
        raise AssertionError(f"EXECOP sequence mismatch. Expected {len(seq)} ops, got {i} ops.")

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
        smp=smp
    )

    make_kstep(linux_dir=linux_dir)

    log_file = LOGS_DIR / f"test_{linux.name}.log"
    out_file = log_file.with_suffix(".out")
    cov_file = log_file.with_suffix(".cov")
    signal_file = log_file.with_suffix(".signal")

    proc = run_qemu(
        linux_dir=linux_dir,
        driver=driver,
        log_file=log_file,
    )

    time.sleep(1)

    # Supported commands:
    # INT,INT,INT,INT: run single operation
    # EXIT: exit the qemu
    payload_str = "\n".join(f"{op[0]},{op[1]},{op[2]},{op[3]}" for op in seq) + "\n"
    pipe_to_qemu(proc=proc, stdin_payload=payload_str)
    pipe_to_qemu(proc=proc, stdin_payload="EXIT\n")

    return_code = proc.wait()
    print(f"QEMU returned with code: {return_code}")

    print_run_results(
        log_file=log_file, 
        out_file=out_file, 
        cov_file=cov_file, 
        signal_file=signal_file,
    )
    assert_exec_matches_generated(log_file, seq)

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
