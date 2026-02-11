#!/usr/bin/env python3

import argparse
import logging

from scripts import generate_input, LOGS_DIR

def main():
    ap = argparse.ArgumentParser(description="Generate a valid kSTEP command sequence")
    ap.add_argument("--steps", type=int, default=100)
    ap.add_argument("--max-tasks", type=int, default=1024)
    ap.add_argument("--max-cgroups", type=int, default=32)
    ap.add_argument("--cpus", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-o", "--out", default="-")

    args = ap.parse_args()

    if args.cpus < 1:
        raise SystemExit("cpus must be >= 1")
    if args.max_tasks < 1:
        raise SystemExit("max-tasks must be >= 1")

    log_path = LOGS_DIR / f"gen_input_{args.seed}.log"
    logging.basicConfig(filename=log_path, encoding="utf-8", level=logging.INFO)

    seq = generate_input(args.steps, args.max_tasks, args.max_cgroups, args.cpus, args.seed)
    
    print(seq)

if __name__ == "__main__":
    main()
