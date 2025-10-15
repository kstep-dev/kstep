#!/usr/bin/env python3

import signal

from scripts import get_linux_dir, system


def run_gdb():
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    linux_dir = get_linux_dir()
    args = [
        f"-iex 'set auto-load safe-path {linux_dir}'",
        "-ex 'target remote :1234'",
        f"-ex 'source {linux_dir}/vmlinux-gdb.py'",
    ]
    system(f"gdb {linux_dir}/vmlinux " + " ".join(args))


if __name__ == "__main__":
    run_gdb()
