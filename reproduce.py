#!/usr/bin/env python3

import argparse
import os
from scripts import PROJ_DIR, LOGS_DIR, get_linux_dir
import time
if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", type=str, default="6.14")
    parser.add_argument("--controller", type=str, default="aa3ee4f")

    args = parser.parse_args()

    linux_dir = get_linux_dir(args.version)

    os.system(f"./fetch_linux.py --versions {args.version}")
    os.system(f"cd {linux_dir} && git restore . && cd -")

    # patched jiffies
    patch_file = f"{PROJ_DIR}/linux/{args.version}-enable-fake-jiffies-in-sched-subsystem.patch"
    patch_cmd = f"cd {linux_dir} && git apply {patch_file} && cd -"
    os.system(patch_cmd)

    # Run the buggy version
    os.system(f"./make_linux.py --versions {args.version}")
    os.system(f"./run_qemu.py --params controller={args.controller} --log_file={LOGS_DIR}/{args.controller}_buggy.log")

    # Run the fixed version
    patch_file = f"{PROJ_DIR}/linux/{args.version}-{args.controller}_fix.patch"
    patch_cmd = f"cd {linux_dir} && git apply {patch_file} && cd -"
    os.system(patch_cmd)

    time.sleep(2)

    os.system(f"./make_linux.py --versions {args.version}")
    os.system(f"./run_qemu.py --params controller={args.controller} --log_file={LOGS_DIR}/{args.controller}_fixed.log")

    # plot the logs
    os.system(f"./plot/plot_cur_task.py --controller={args.controller}")
