#!/usr/bin/env python3
"""Concurrent coverage-guided fuzzer for kSTEP.

Architecture:
  Manager  – single process owning the global SignalCorpus and seed pool.
             Distributes work via task_queue, collects results via result_queue,
             updates corpus, and enqueues a new item after every result.
  Workers  – N parallel QEMU instances.  Each worker runs one test per loop
             iteration (fresh random generation or exact seed replay)
             and returns the executed op sequence plus the path to its .cov file.

Seed scheduling:
  Every input that discovers new signals becomes a seed.
  Seeds are replayed in round-robin order.  The fresh_ratio controls what
  fraction of iterations use fresh random generation vs. seed replay.

Usage:
  python fuzz.py --linux_name v6.18_test [options]
"""

import argparse
import logging
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from run import Driver, make_kstep
from scripts.consts import FUZZ_DIR
from scripts.fuzz_manager import run_manager


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Concurrent coverage-guided fuzzer for kSTEP",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--linux_name", required=True,
                        help="Linux build name, e.g. v6.18_test")
    parser.add_argument("--num_cpus", type=int, default=5,
                        help="vCPUs per QEMU instance")
    parser.add_argument("--mem_mb", type=int, default=512,
                        help="RAM per QEMU instance (MB)")
    parser.add_argument(
        "--topology",
        type=str,
        default="CLS:0/1-2/1-2/3-4/3-4",
        help="Executor topology passed as a module param, e.g. CLS:0/1-2/1-2/3-4/3-4",
    )
    parser.add_argument(
        "--frequency",
        type=str,
        default=None,
        help="Executor per-CPU frequency scale passed as a module param, e.g. 1024/512/1024/512/1024",
    )
    parser.add_argument("--workers", type=int,
                        default=1,
                        help="Number of parallel QEMU workers")
    parser.add_argument("--steps", type=int, default=50,
                        help="Ops per test case (fresh mode)")
    parser.add_argument("--fresh_ratio", type=float, default=0.1,
                        help="Fraction of iterations using fresh random generation")
    parser.add_argument("--mutate_ratio", type=float, default=0.9,
                        help="Fraction of iterations applying tick-insertion mutation to a seed "
                             "(remainder is pure replay)")
    parser.add_argument(
        "--special_mutate_ratio",
        type=float,
        default=0.2,
        help="Within mutate mode, chance to choose from the special-state seed pool before the coverage pool",
    )
    parser.add_argument(
        "--pivot_rarity_alpha",
        type=float,
        default=2.0,
        help="Rarity exponent for productive pivot selection; <= 0 keeps pivots near-uniform",
    )
    parser.add_argument(
        "--pin_cpus",
        type=str,
        default=None,
        metavar="CPUSET",
        help="Pin QEMU workers and host-side Python processes to CPUs in CPUSET, e.g. 0-9 or 0-3,8-11",
    )
    parser.add_argument(
        "--cross_scheduler",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="Enable cross-scheduler fuzzing; sets TASK_FIFO and TASK_CFS weights to 2 instead of 0",
    )
    parser.add_argument(
        "--ci_mode",
        action="store_true",
        help="Run a bounded CI smoke test: exactly 3 executions in fresh, replay, then mutate mode",
    )
    args = parser.parse_args()

    FUZZ_DIR.mkdir(parents=True, exist_ok=True)

    # Set up file logging — all logging.* calls go to both console and this file.
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = FUZZ_DIR / f"fuzz_{timestamp}.log"
    file_handler = logging.FileHandler(log_path)
    file_handler.setFormatter(logging.Formatter("[%(asctime)s] %(message)s", datefmt="%H:%M:%S"))
    logging.getLogger().addHandler(file_handler)
    logging.info(f"Logging to {log_path}")

    make_kstep(args.linux_name)

    driver = Driver(
        name="executor",
        num_cpus=args.num_cpus,
        mem_mb=args.mem_mb,
        topology=args.topology,
        frequency=args.frequency,
    )

    logging.info(
        f"kSTEP fuzzer: workers={args.workers}  driver=executor  "
        f"linux={args.linux_name}  steps={args.steps}  "
        f"topology={args.topology or 'default'}  "
        f"frequency={args.frequency or 'default'}  "
        f"ci_mode={args.ci_mode}  "
        f"cross_scheduler={args.cross_scheduler}  "
        f"fresh_ratio={args.fresh_ratio}  mutate_ratio={args.mutate_ratio}  "
        f"special_mutate_ratio={args.special_mutate_ratio}  "
        f"pivot_rarity_alpha={args.pivot_rarity_alpha}"
    )
    run_manager(
        n_workers=args.workers,
        driver=driver,
        linux_name=args.linux_name,
        steps=args.steps,
        fresh_ratio=args.fresh_ratio,
        mutate_ratio=args.mutate_ratio,
        special_mutate_ratio=args.special_mutate_ratio,
        pivot_rarity_alpha=args.pivot_rarity_alpha,
        cross_scheduler=args.cross_scheduler,
        pin_cpus=args.pin_cpus,
        ci_mode=args.ci_mode,
    )


if __name__ == "__main__":
    main()
