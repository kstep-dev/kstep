#!/usr/bin/env python3

import argparse
import signal
from pathlib import Path

from scripts import get_linux_dir, system


def run_gdb(linux_dir: Path):
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    args = [
        f"-iex 'set auto-load safe-path {linux_dir}'",
        "-ex 'target remote :1234'",
        f"-ex 'source {linux_dir}/vmlinux-gdb.py'",
    ]
    system(f"gdb {linux_dir}/vmlinux " + " ".join(args))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux_dir", type=Path, default=get_linux_dir())
    args = parser.parse_args()
    run_gdb(**vars(args))
