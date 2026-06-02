#!/usr/bin/env python3

import argparse
import multiprocessing as mp

from checkout import Linux, checkout
from run import Driver, make_kstep, make_linux
from scripts import GLOBAL_SIGNAL_CORPUS, cov_parse
from scripts.fuzz_common import WorkItem, worker_dir
from scripts.fuzz_worker import FuzzWorker
from scripts.input_seq import InputSeq, input_seq_from_log
from scripts.utils import ResultDir
    
def run_test(
    linux: Linux,
    num_cpus: int,
    steps: int,
    seed: int,
):
    name = linux.name
    checkout(linux.version, name=linux.name, tarball=True)
    make_linux(name=name)

    driver = Driver(
        name="executor",
        num_cpus=num_cpus,
    )

    make_kstep(name=name)
    task_queue: "mp.Queue[WorkItem | None]" = mp.Queue()
    result_queue: "mp.Queue" = mp.Queue()
    task_queue.put(WorkItem(mode="fresh", steps=steps))
    task_queue.put(None)

    worker = FuzzWorker(
        worker_id = 0,
        task_queue = task_queue,
        result_queue = result_queue,
        driver = driver,
        name = name,
        rng_seed = seed,
        base_dir = ResultDir.create().path
    )
    worker.run()

    result = result_queue.get(timeout=5)
    if result.error:
        raise RuntimeError(f"worker failed: {result.error_category}: {result.error}")

    wdir = worker_dir(0)
    seq = InputSeq()
    for op in result.ops:
        seq.append(op)
    seq2 = input_seq_from_log(wdir.log)
    assert seq == seq2

    # Parse the signal file and get the signal records and list
    signal_records = cov_parse(wdir.cov)

    # Symbolize the pcs
    GLOBAL_SIGNAL_CORPUS.update_pc_symbolize(
        signal_records=signal_records,
        name=linux.name,
    )

    # Analyze the new signals for the test
    new_signal_info = GLOBAL_SIGNAL_CORPUS.analyze_new_signals(
        seq=seq,
        signal_records=signal_records,
        name=linux.name,
    )

    # Analyze the per-action signals for the test if there are new signals
    if new_signal_info:
        new_signals, _ = new_signal_info
        GLOBAL_SIGNAL_CORPUS.analyze_per_action_signals(
            seq=seq,
            signal_records=signal_records,
            new_signals=new_signals,
            name=linux.name,
        )
    
    return

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_version", type=str, default="master")
    parser.add_argument("--num_cpus", type=int, default=3)
    parser.add_argument("--steps", type=int, default=80)
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
        seed=args.seed,
    )

if __name__ == "__main__":
    main()
