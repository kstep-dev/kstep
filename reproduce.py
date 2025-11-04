#!/usr/bin/env python3

import argparse
import time

from scripts import LOGS_DIR, PROJ_DIR, get_linux_dir, system

versions_map = {
    "aa3ee4f": "6.14",
    "cd9626e": "6.12-rc3",
}
if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="aa3ee4f")
    parser.add_argument("--clean", action="store_true", default=False)

    args = parser.parse_args()

    version = versions_map.get(args.controller)
    if not version:
        parser.error(f"controller '{args.controller}' not found in versions_map. Valid options are: {list(versions_map.keys())}")
    linux_dir = get_linux_dir(version)

    # Clean up old kmod build as it may conflicts with the new kernel build
    if args.clean:
        system(f"make clean")

    system(f"./fetch_linux.py --versions {version}")
    system(f"cd {linux_dir} && git restore . && cd -")

    # patched initial min_vruntime
    patch_file = f"{PROJ_DIR}/linux/{version}-vruntime_min_init.patch"
    patch_cmd = f"cd {linux_dir} && git apply {patch_file} && cd -"
    system(patch_cmd)

    # Run the buggy version
    system(f"./make_linux.py --versions {version}")
    system(
        f"./run_qemu.py --params controller={args.controller} --log_file={LOGS_DIR}/{args.controller}_buggy.log"
    )

    # Run the fixed version
    patch_file = f"{PROJ_DIR}/linux/{version}-{args.controller}_fix.patch"
    patch_cmd = f"cd {linux_dir} && git apply {patch_file} && cd -"
    system(patch_cmd)

    time.sleep(2)

    system(f"./make_linux.py --versions {version}")
    system(
        f"./run_qemu.py --params controller={args.controller} --log_file={LOGS_DIR}/{args.controller}_fixed.log"
    )

    # plot the logs
    system(f"./plot/plot_cur_task.py --controller={args.controller}")
