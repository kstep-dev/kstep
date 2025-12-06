#!/usr/bin/env -S uv run --script

import argparse
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List

from checkout_linux import checkout_linux
from make_linux import make_linux
from run_qemu import make_kstep, run_qemu
from scripts import LINUX_ROOT_DIR, PROJ_DIR, RESULTS_DIR, system

# Enforce reproducible builds for matplotlib PDF output
# https://reproducible-builds.org/docs/source-date-epoch/
os.environ["SOURCE_DATE_EPOCH"] = "0"


@dataclass(frozen=True)
class Config:
    # The version/commit of the kernel to use
    version: str
    # The patches to apply to the kernel
    patches: Iterable[Path] = ()
    # The parameters to pass to the kernel module
    params: Iterable[str] = ()


@dataclass(frozen=True)
class Bug:
    name: str
    plot_format: str
    fixed: Config
    buggy: Config
    smp: str = "3"
    mem_mb: int = 256


bugs = [
    # https://github.com/torvalds/linux/commit/aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042
    Bug(
        name="sync_wakeup",
        plot_format="cur_task",
        buggy=Config(version="v6.14"),
        fixed=Config(version="v6.14", patches=[LINUX_ROOT_DIR / "sync_wakeup.patch"]),
    ),
    # https://github.com/torvalds/linux/commit/cd9626e9ebc77edec33023fe95dab4b04ffc819d
    Bug(
        name="freeze",
        plot_format="cur_task",
        fixed=Config(version="cd9626e"),
        buggy=Config(version="cd9626e~1"),
        smp="2",
    ),
    # https://github.com/torvalds/linux/commit/bbce3de72be56e4b5f68924b7da9630cc89aa1a8
    Bug(
        name="vruntime_overflow",
        plot_format="cur_task",
        fixed=Config(version="bbce3de"),
        buggy=Config(version="bbce3de~1"),
        smp="2",
    ),
    # https://github.com/torvalds/linux/commit/2feab2492deb2f14f9675dd6388e9e2bf669c27a
    Bug(
        name="long_balance",
        plot_format="rebalance",
        mem_mb=25600,
        fixed=Config(version="2feab24"),
        buggy=Config(version="2feab24~1"),
    ),
    # https://github.com/torvalds/linux/commit/17e3e88ed0b6318fde0d1c14df1a804711cab1b5
    Bug(
        name="util_avg",
        plot_format="util_avg",
        fixed=Config(version="17e3e88"),
        buggy=Config(version="17e3e88~1"),
        smp="2",
    ),
    # https://github.com/torvalds/linux/commit/5068d84054b766efe7c6202fc71b2350d1c326f1
    Bug(
        name="lag_vruntime",
        plot_format="min_vruntime",
        fixed=Config(version="5068d84"),
        buggy=Config(version="5068d84~1"),
    ),
    Bug(
        name="even_idle_cpu",
        plot_format="nr_running",
        smp="8",
        fixed=Config(version="v6.14"),
        buggy=Config(version="v6.14", params=["special_topo=true"]),
    ),
    # https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3
    Bug(
        name="extra_balance",
        plot_format="lb_nr_running",
        smp="8,sockets=2,cores=2,threads=2",
        fixed=Config(version="6d7e478"),
        buggy=Config(version="6d7e478~1"),
    ),
]


def patch_linux(linux_dir: Path, patch_file: Path):
    system(
        f"(cd {linux_dir} && git apply {patch_file}) || "
        f"(cd {linux_dir} && git apply {patch_file} --reverse --check && echo '{patch_file} already applied')"
    )


def plot_data(python_script: str, controller: str):
    system(f"{PROJ_DIR}/scripts/plot_{python_script}.py --controller={controller}")


def main(bug: Bug, run: List[str], reset: bool):
    # Run the buggy version
    if "buggy" in run:
        linux_dir = LINUX_ROOT_DIR / f"{bug.name}_buggy"
        checkout_linux(bug.buggy.version, linux_dir=linux_dir, reset=reset)
        for patch in bug.buggy.patches:
            patch_linux(linux_dir, patch)
        make_linux(linux_dir)
        make_kstep()
        run_qemu(
            linux_dir=linux_dir,
            driver=bug.name,
            params=bug.buggy.params,
            log_file=RESULTS_DIR / f"{bug.name}_buggy.log",
            smp=bug.smp,
            mem_mb=bug.mem_mb,
        )

    # Run the fixed version
    if "fixed" in run:
        linux_dir = LINUX_ROOT_DIR / f"{bug.name}_fixed"
        checkout_linux(bug.fixed.version, linux_dir=linux_dir, reset=reset)
        for patch in bug.fixed.patches:
            patch_linux(linux_dir, patch)
        make_linux(linux_dir)
        make_kstep()
        run_qemu(
            linux_dir=linux_dir,
            driver=bug.name,
            params=bug.fixed.params,
            log_file=RESULTS_DIR / f"{bug.name}_fixed.log",
            smp=bug.smp,
            mem_mb=bug.mem_mb,
        )

    # plot the logs
    if "plot" in run:
        plot_data(bug.plot_format, bug.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "name",
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

    if args.name == "all":
        for bug in bugs:
            main(bug=bug, run=args.run, reset=args.reset)
    else:
        bug = next((bug for bug in bugs if bug.name == args.name), None)
        if not bug:
            raise ValueError(f"Bug '{args.name}' not found.")
        main(bug=bug, run=args.run, reset=args.reset)
