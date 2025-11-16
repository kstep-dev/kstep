#!/usr/bin/env python3

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from checkout_linux import checkout_linux
from make_linux import make_linux
from run_qemu import run_qemu
from scripts import LINUX_ROOT_DIR, LOGS_DIR, PROJ_DIR, system


@dataclass(frozen=True)
class Bug:
    name: str
    version: str
    plot_format: str
    smp: str = "cpus=3,cores=3"
    mem_mb: int = 256
    fix_patch_file: Optional[str] = None


bugs = [
    Bug(name="aa3ee4f", version="v6.14", plot_format="cur_task"),
    Bug(name="cd9626e", version="v6.12-rc3", plot_format="cur_task"),
    Bug(name="bbce3de", version="v6.14", plot_format="cur_task"),
    Bug(name="2feab24", version="v6.9", plot_format="rebalance", mem_mb=25600),
    Bug(name="17e3e88", version="v6.9", plot_format="util_avg"),
    Bug(name="5068d84", version="v6.7", plot_format="min_vruntime"),
    Bug(
        name="evenIdleCpu",
        version="v6.7-rc1",
        plot_format="nr_running",
        smp="8,dies=4,cores=2,threads=1",
        fix_patch_file="use_special_topo.patch",
    ),
    Bug(
        name="6d7e478",
        version="v6.7-rc1",
        plot_format="lb_nr_running",
        smp="8,sockets=2,cores=2,threads=2",
    ),
]

def patch_linux(linux_dir: Path, patch_file: Path):
    system(f"cd {linux_dir} && git apply {patch_file}")


def reset_git(linux_dir: Path):
    system(f"cd {linux_dir} && git restore .")


def plot_data(python_script: str, controller: str):
    system(f"{PROJ_DIR}/plot/plot_{python_script}.py --controller={controller}")


def main(bug: Bug):
    linux_dir = LINUX_ROOT_DIR / bug.version

    checkout_linux(bug.version, linux_dir=linux_dir)
    reset_git(linux_dir)

    # patched initial min_vruntime
    # Select appropriate patch file based on plot format type
    if bug.plot_format == "rebalance":
        suffix = "-vruntime_min_init-trace_rebalance.patch"
    elif bug.plot_format == "lb_nr_running":
        suffix = "-vruntime_min_init-trace-lb.patch"
    else:
        suffix = "-vruntime_min_init.patch"
    patch_file = f"{PROJ_DIR}/linux/{bug.version}{suffix}"
    patch_linux(linux_dir, patch_file)

    # Run the buggy version
    make_linux(linux_dir)
    run_qemu(
        linux_dir=linux_dir,
        params=[f"controller={bug.name}"],
        log_file=LOGS_DIR / f"{bug.name}_buggy.log",
        smp=bug.smp,
        mem_mb=bug.mem_mb,
    )

    # Run the fixed version
    # for the new bug evenIdleCpu, we use the patch that generate special topo trigger the bug.
    if not bug.fix_patch_file:
        patch_file = f"{PROJ_DIR}/linux/{bug.version}-{bug.name}_fix.patch"
    else:
        patch_file = f"{PROJ_DIR}/linux/{bug.version}-{bug.fix_patch_file}"
    patch_linux(linux_dir, patch_file)

    make_linux(linux_dir)
    run_qemu(
        linux_dir=linux_dir,
        params=[f"controller={bug.name}"],
        log_file=LOGS_DIR / f"{bug.name}_fixed.log",
        smp=bug.smp,
        mem_mb=bug.mem_mb,
    )

    # plot the logs
    plot_data(bug.plot_format, bug.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="aa3ee4f")
    args = parser.parse_args()

    bug = next((bug for bug in bugs if bug.name == args.controller), None)
    if not bug:
        raise ValueError(f"controller '{args.controller}' not found in bugs.")
    main(bug)
