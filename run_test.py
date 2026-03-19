#!/usr/bin/env python3

import argparse
import socket
import time

from checkout_linux import Linux, checkout_linux
from run import (
    Driver,
    make_kstep,
    make_linux,
    start_qemu,
)
from scripts import LOGS_DIR, GLOBAL_SIGNAL_CORPUS, cov_parse, input_seq_from_log
from scripts.gen_input_core import init_genstate, generate_next_command
from scripts.input_seq import InputSeq
from scripts.gen_input_ops import OP_NAME_TO_TYPE
    
def run_test(
    linux: Linux,
    num_cpus: int,
    steps: int,
    max_tasks: int,
    max_cgroups: int,
    seed: int,
):
    linux_name = linux.name
    checkout_linux(linux.version, linux_name=linux_name, tarball=True)
    make_linux(linux_name=linux_name)

    driver = Driver(
        name="executor",
        num_cpus=num_cpus,
    )

    make_kstep(linux_name=linux_name)

    log_file = LOGS_DIR / f"{linux_name}.log"
    cov_file = log_file.with_suffix(".cov")
    sock_file = log_file.with_suffix(".sock")
    proc = start_qemu(
        linux_name=linux_name,
        driver=driver,
        log_file=log_file,
        sock_file=sock_file,
    )

    # Connect to the socket. QEMU blocks (wait=on) until we connect, so retry
    # until the socket file appears.
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(60):
        try:
            sock.connect(str(sock_file))
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.1)
    else:
        raise RuntimeError(f"Timed out connecting to {sock_file}")

    sf = sock.makefile("rb")
    OP_TYPE_NR = 16  # marker byte written by kstep_write_state

    def read_state() -> list[dict]:
        """Read lines until a state message arrives, discarding TTY echo.
        Returns list of {"id": int, "state": int} dicts."""
        while True:
            line = sf.readline()
            if not line:
                raise EOFError("kmod socket closed (QEMU exited)")
            if line[0] == OP_TYPE_NR:
                payload = line[1:-1]   # strip marker byte and trailing '\n'
                return [{"id": payload[i], "state": payload[i + 1]}
                        for i in range(0, len(payload), 2)]

    gen = init_genstate(max_tasks=max_tasks, max_cgroups=max_cgroups, cpus=num_cpus, seed=seed)

    # Wait for the initial STATE from the kmod, signalling it is ready.
    task_states = read_state()
    print(f"kmod ready: {task_states}")

    seq = InputSeq()
    for _ in range(steps):
        op, a, b, c = generate_next_command(gen, task_states)
        if op == OP_NAME_TO_TYPE["TICK_REPEAT"]:
            for _ in range(a):
                seq.append((OP_NAME_TO_TYPE["TICK"], 0, 0, 0))
        else:
            seq.append((op, a, b, c))
        sock.sendall(f"{op},{a},{b},{c}\n".encode())
        task_states = read_state()
        gen.update_from_kmod(task_states)

    sock.sendall(b"EXIT\n")
    sock.close()
    proc.wait()


    # assert_exec_matches_generated(log_file, seq)
    seq2 = input_seq_from_log(log_file)
    print(len(seq), len(seq2))
    assert seq == seq2

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
    parser.add_argument("--num_cpus", type=int, default=3)
    parser.add_argument("--steps", type=int, default=80)
    parser.add_argument("--max_tasks", type=int, default=10)
    parser.add_argument("--max_cgroups", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    linux = Linux(
        name=f"{args.linux_version}_test",
        version=args.linux_version,
    )

    run_test(
        linux=linux,
        num_cpus=args.num_cpus,
        steps=args.steps,
        max_tasks=args.max_tasks,
        max_cgroups=args.max_cgroups,
        seed=args.seed,
    )

if __name__ == "__main__":
    main()
