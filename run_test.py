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
from scripts import GLOBAL_SIGNAL_CORPUS, LOGS_DIR, cov_parse, generate_input


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
    linux_name = linux.name
    checkout_linux(linux.version, linux_name=linux_name, tarball=True)
    make_linux(linux_name=linux_name)

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

    make_kstep(linux_name=linux_name)

    log_file = LOGS_DIR / f"{linux_name}.log"
    out_file = log_file.with_suffix(".out")
    cov_file = log_file.with_suffix(".cov")

    proc = run_qemu(
        linux_name=linux_name,
        driver=driver,
        log_file=log_file,
    )

    time.sleep(1)

    # Supported commands:
    # INT,INT,INT,INT: run single operation
    # EXIT: exit the qemu
    payload_str = seq.to_payload()
    pipe_to_qemu(proc=proc, stdin_payload=payload_str)
    pipe_to_qemu(proc=proc, stdin_payload="EXIT\n")

    return_code = proc.wait()
    print(f"QEMU returned with code: {return_code}")

    assert_exec_matches_generated(log_file, seq)

    # Parse the signal file and get the signal records and list
    signal_records = cov_parse(cov_file)

    # Symbolize the pcs
    GLOBAL_SIGNAL_CORPUS.update_pc_symbolize(
        signal_records=signal_records,
        linux_name=linux.name,
    )

    # Analyze the new signals for the test
    new_signals = GLOBAL_SIGNAL_CORPUS.analyze_new_signals(
        seq=seq,
        signal_records=signal_records,
        linux_name=linux.name,
    )

    # Analyze the per-action signals for the test if there are new signals
    if new_signals:
        GLOBAL_SIGNAL_CORPUS.analyze_per_action_signals(
            seq=seq,
            signal_records=signal_records,
            new_signals=new_signals,
            linux_name=linux.name,
        )
    
    return

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_version", type=str, default="v6.18")
    parser.add_argument("--smp", type=str, default="10")
    parser.add_argument("--steps", type=int, default=80)
    parser.add_argument("--max_tasks", type=int, default=10)
    parser.add_argument("--max_cgroups", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    linux = Linux(
        name=f"{args.linux_name}_test",
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
