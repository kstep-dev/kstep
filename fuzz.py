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
import os
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from run import Driver
from scripts.consts import DATA_DIR
from scripts.fuzz_manager import run_manager


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Concurrent coverage-guided fuzzer for kSTEP",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--linux_name", required=True,
                        help="Linux build name, e.g. v6.18_test")
    parser.add_argument("--driver", default="executor",
                        help="kSTEP driver name")
    parser.add_argument("--num_cpus", type=int, default=3,
                        help="vCPUs per QEMU instance")
    parser.add_argument("--mem_mb", type=int, default=512,
                        help="RAM per QEMU instance (MB)")
    parser.add_argument("--workers", type=int,
                        default=max(1, (os.cpu_count() or 4) // 4),
                        help="Number of parallel QEMU workers")
    parser.add_argument("--steps", type=int, default=80,
                        help="Ops per test case (fresh mode)")
    parser.add_argument("--fresh_ratio", type=float, default=0.5,
                        help="Fraction of iterations using fresh random generation")
    parser.add_argument("--mutate_ratio", type=float, default=0.5,
                        help="Fraction of iterations applying tick-insertion mutation to a seed "
                             "(remainder is pure replay)")
    parser.add_argument("--pin_cpus", action="store_true",
                        help="Pin each QEMU to dedicated host CPUs; run Python on the rest")
    args = parser.parse_args()

    fuzz_dir = DATA_DIR / "fuzz"
    fuzz_dir.mkdir(parents=True, exist_ok=True)

    # Set up file logging — all logging.* calls go to both console and this file.
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = fuzz_dir / f"fuzz_{timestamp}.log"
    file_handler = logging.FileHandler(log_path)
    file_handler.setFormatter(logging.Formatter("[%(asctime)s] %(message)s", datefmt="%H:%M:%S"))
    logging.getLogger().addHandler(file_handler)
    logging.info(f"Logging to {log_path}")

    driver = Driver(name=args.driver, num_cpus=args.num_cpus, mem_mb=args.mem_mb)

    logging.info(
        f"kSTEP fuzzer: workers={args.workers}  driver={args.driver}  "
        f"linux={args.linux_name}  steps={args.steps}  "
        f"fresh_ratio={args.fresh_ratio}  mutate_ratio={args.mutate_ratio}"
    )
    run_manager(
        n_workers=args.workers,
        driver=driver,
        linux_name=args.linux_name,
        steps=args.steps,
        fuzz_dir=fuzz_dir,
        fresh_ratio=args.fresh_ratio,
        mutate_ratio=args.mutate_ratio,
        pin_cpus=args.pin_cpus,
    )


if __name__ == "__main__":
    main()
