#!/usr/bin/env python3

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List

from checkout_linux import checkout_linux
from make_linux import make_linux
from run_qemu import make_kstep, run_qemu
from scripts import LINUX_ROOT_DIR, LOGS_DIR, PROJ_DIR, system


@dataclass
class Bug:
    name: str
    plot_format: str
    version_fixed: str
    version_buggy: str = ""
    patch_files_fixed: Iterable[Path] = ()
    patch_files_buggy: Iterable[Path] = ()
    params_fixed: Iterable[str] = ()
    params_buggy: Iterable[str] = ()
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
        version_fixed="v6.14",
        version_buggy="v6.14",
        plot_format="nr_running",
        smp="8",
        params_buggy=["special_topo=true"],
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


def main(bug: Bug, run: List[str], reset: bool):
    make_kstep()

    # Run the buggy version
    if "buggy" in run:
        linux_dir = LINUX_ROOT_DIR / f"{bug.name}_buggy"
        checkout_linux(bug.version_buggy, linux_dir=linux_dir, reset=reset)
        for patch_file in bug.patch_files_buggy:
            patch_linux(linux_dir, patch_file)
        make_linux(linux_dir)
        run_qemu(
            linux_dir=linux_dir,
            controller=bug.name,
            params=bug.params_buggy,
            log_file=LOGS_DIR / f"{bug.name}_buggy.log",
            smp=bug.smp,
            mem_mb=bug.mem_mb,
        )

    # Run the fixed version
    if "fixed" in run:
        linux_dir = LINUX_ROOT_DIR / f"{bug.name}_fixed"
        checkout_linux(bug.version_fixed, linux_dir=linux_dir, reset=reset)
        for patch_file in bug.patch_files_fixed:
            patch_linux(linux_dir, patch_file)
        make_linux(linux_dir)
        run_qemu(
            linux_dir=linux_dir,
            controller=bug.name,
            params=bug.params_fixed,
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
    parser.add_argument("--reset", action="store_true", default=False)
    args = parser.parse_args()

    if args.controller == "all":
        for bug in bugs:
            main(bug=bug, run=args.run, reset=args.reset)
    else:
        bug = next((bug for bug in bugs if bug.name == args.controller), None)
        if not bug:
            raise ValueError(f"controller '{args.controller}' not found in bugs.")
        main(bug=bug, run=args.run, reset=args.reset)
