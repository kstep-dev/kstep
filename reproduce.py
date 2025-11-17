#!/usr/bin/env python3

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from checkout_linux import checkout_linux
from make_linux import make_linux
from run_qemu import run_qemu
from scripts import LINUX_ROOT_DIR, LOGS_DIR, PROJ_DIR, system


@dataclass
class Bug:
    name: str
    plot_format: str
    version_fixed: str
    version_buggy: str = ""
    patch_file_fixed: Optional[Path] = None
    patch_file_buggy: Optional[Path] = None
    smp: str = "cpus=3,cores=3"
    mem_mb: int = 256

    def __post_init__(self):
        # Default to the previous one of the fixed version
        if self.version_buggy == "":
            self.version_buggy = f"{self.version_fixed}~1"


bugs = [
    Bug(name="aa3ee4f", version_fixed="aa3ee4f", plot_format="cur_task"),
    Bug(name="cd9626e", version_fixed="cd9626e", plot_format="cur_task"),
    Bug(name="bbce3de", version_fixed="bbce3de", plot_format="cur_task"),
    Bug(name="2feab24", version_fixed="2feab24", plot_format="rebalance", mem_mb=25600),
    Bug(name="17e3e88", version_fixed="17e3e88", plot_format="util_avg"),
    Bug(name="5068d84", version_fixed="5068d84", plot_format="min_vruntime"),
    Bug(
        name="evenIdleCpu",
        version_fixed="v6.7-rc1",
        version_buggy="v6.7-rc1",
        plot_format="nr_running",
        smp="8,dies=4,cores=2,threads=1",
        # We use the patch that generate special topo to trigger the bug.
        patch_file_buggy=LINUX_ROOT_DIR / "v6.7-rc1-use_special_topo.patch",
    ),
    Bug(
        name="6d7e478",
        version_fixed="6d7e478",
        plot_format="lb_nr_running",
        smp="8,sockets=2,cores=2,threads=2",
    ),
]


def patch_linux(linux_dir: Path, patch_file: Path):
    system(
        f"(cd {linux_dir} && git apply {patch_file}) || "
        f"(cd {linux_dir} && git apply {patch_file} --reverse --check && echo '{patch_file} already applied')"
    )


def plot_data(python_script: str, controller: str):
    system(f"{PROJ_DIR}/plot/plot_{python_script}.py --controller={controller}")


def main(bug: Bug, run: List[str]):
    # Run the buggy version
    if "buggy" in run:
        linux_dir = LINUX_ROOT_DIR / f"{bug.name}_buggy"
        checkout_linux(bug.version_buggy, linux_dir=linux_dir, reset=True)
        if bug.patch_file_buggy:
            patch_linux(linux_dir, bug.patch_file_buggy)
        make_linux(linux_dir)
        run_qemu(
            linux_dir=linux_dir,
            controller=bug.name,
            log_file=LOGS_DIR / f"{bug.name}_buggy.log",
            smp=bug.smp,
            mem_mb=bug.mem_mb,
        )

    # Run the fixed version
    if "fixed" in run:
        linux_dir = LINUX_ROOT_DIR / f"{bug.name}_fixed"
        checkout_linux(bug.version_fixed, linux_dir=linux_dir, reset=True)
        if bug.patch_file_fixed:
            patch_linux(linux_dir, bug.patch_file_fixed)
        make_linux(linux_dir)
        run_qemu(
            linux_dir=linux_dir,
            controller=bug.name,
            log_file=LOGS_DIR / f"{bug.name}_fixed.log",
            smp=bug.smp,
            mem_mb=bug.mem_mb,
        )

    # plot the logs
    if "plot" in run:
        plot_data(bug.plot_format, bug.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--controller",
        type=str,
        default="all",
        choices=["all", *[bug.name for bug in bugs]],
        help="The name of the bug to reproduce, or 'all' to reproduce all bugs.",
    )
    parser.add_argument(
        "--run",
        type=str,
        default=["buggy", "fixed", "plot"],
        choices=["buggy", "fixed", "plot"],
        nargs="+",
    )
    args = parser.parse_args()

    if args.controller == "all":
        for bug in bugs:
            main(bug=bug, run=args.run)
    else:
        bug = next((bug for bug in bugs if bug.name == args.controller), None)
        if not bug:
            raise ValueError(f"controller '{args.controller}' not found in bugs.")
        main(bug=bug, run=args.run)
