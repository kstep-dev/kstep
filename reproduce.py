#!/usr/bin/env -S uv run --script

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List

from checkout_linux import checkout_linux
from run import Driver, make_kstep, make_linux, run_qemu
from scripts import LINUX_ROOT_DIR, PROJ_DIR, RESULTS_DIR, system


@dataclass(frozen=True)
class Linux:
    # A descriptive name for the Linux version
    name: str
    # The version/commit of the kernel to use
    version: str
    # The patches to apply to the kernel
    patches: Iterable[Path] = ()


@dataclass(frozen=True)
class Bug:
    driver: Driver
    linux: List[Linux]
    plot_format: str


bugs = [
    # https://github.com/torvalds/linux/commit/aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042
    Bug(
        driver=Driver(name="sync_wakeup", smp="3"),
        linux=[
            Linux(name="buggy", version="v6.14"),
            Linux(
                name="fixed",
                version="v6.14",
                patches=[LINUX_ROOT_DIR / "sync_wakeup.patch"],
            ),
        ],
        plot_format="cur_task",
    ),
    # https://github.com/torvalds/linux/commit/cd9626e9ebc77edec33023fe95dab4b04ffc819d
    Bug(
        driver=Driver(name="freeze", smp="2"),
        linux=[
            Linux(name="buggy", version="cd9626e~1"),
            Linux(name="fixed", version="cd9626e"),
        ],
        plot_format="cur_task",
    ),
    # https://github.com/torvalds/linux/commit/bbce3de72be56e4b5f68924b7da9630cc89aa1a8
    Bug(
        driver=Driver(name="vruntime_overflow", smp="2"),
        linux=[
            Linux(name="buggy", version="bbce3de~1"),
            Linux(name="fixed", version="bbce3de"),
        ],
        plot_format="cur_task",
    ),
    # https://github.com/torvalds/linux/commit/2feab2492deb2f14f9675dd6388e9e2bf669c27a
    Bug(
        driver=Driver(name="long_balance", smp="3", mem_mb=4096),
        linux=[
            Linux(name="buggy", version="2feab24~1"),
            Linux(name="fixed", version="2feab24"),
        ],
        plot_format="rebalance",
    ),
    # https://github.com/torvalds/linux/commit/17e3e88ed0b6318fde0d1c14df1a804711cab1b5
    Bug(
        driver=Driver(name="util_avg", smp="2"),
        linux=[
            Linux(name="buggy", version="17e3e88~1"),
            Linux(name="fixed", version="17e3e88"),
        ],
        plot_format="util_avg",
    ),
    # https://github.com/torvalds/linux/commit/5068d84054b766efe7c6202fc71b2350d1c326f1
    Bug(
        driver=Driver(name="lag_vruntime", smp="2"),
        plot_format="min_vruntime",
        linux=[
            Linux(name="buggy", version="5068d84~1"),
            Linux(name="fixed", version="5068d84"),
        ],
    ),
    Bug(
        driver=Driver(name="even_idle_cpu", smp="5"),
        linux=[
            Linux(name="buggy", version="v6.17"),
            Linux(
                name="fixed",
                version="v6.17",
                patches=[LINUX_ROOT_DIR / "even_idle_cpu.patch"],
            ),
        ],
        plot_format="lb_nr_running",
    ),
    # https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3
    Bug(
        driver=Driver(name="extra_balance", smp="8,sockets=2,cores=2,threads=2"),
        linux=[
            Linux(name="buggy", version="6d7e478~1"),
            Linux(name="fixed", version="6d7e478"),
        ],
        plot_format="lb_nr_running",
    ),
]


def patch_linux(linux_dir: Path, patch_file: Path):
    system(
        f"(cd {linux_dir} && git apply {patch_file}) || "
        f"(cd {linux_dir} && git apply {patch_file} --reverse --check && echo '{patch_file} already applied')"
    )


def reset_git(linux_dir: Path):
    system(f"cd {linux_dir} && git restore .")


def plot_data(python_script: str, controller: str):
    system(f"{PROJ_DIR}/scripts/plot_{python_script}.py --controller={controller}")


def reproduce(linux: Linux, driver: Driver, reset: bool, skip_build: bool):
    if linux.patches:
        linux_dir = LINUX_ROOT_DIR / f"{driver.name}_{linux.name}"
    else:
        linux_dir = LINUX_ROOT_DIR / linux.version
    checkout_linux(linux.version, linux_dir=linux_dir)
    if reset:
        reset_git(linux_dir)
    for patch in linux.patches:
        patch_linux(linux_dir, patch)
    if not skip_build:
        make_linux()
    make_kstep()
    log_file = RESULTS_DIR / f"{driver.name}_{linux.name}.log"
    run_qemu(linux_dir=linux_dir, driver=driver, log_file=log_file)


def main(bug: Bug, run: List[str], reset: bool, skip_build: bool):
    linux_map = {linux.name: linux for linux in bug.linux}
    for r in [r for r in run if r != "plot"]:
        linux = linux_map.get(r, Linux(name=r, version=r))
        reproduce(linux, bug.driver, reset, skip_build)

    if "plot" in run:
        plot_data(bug.plot_format, bug.driver.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "name",
        type=str,
        default="all",
        choices=["all", *[bug.driver.name for bug in bugs]],
        help="The name of the bug to reproduce, or 'all' to reproduce all bugs.",
    )
    parser.add_argument(
        "--run",
        type=str,
        default=["buggy", "fixed", "plot"],
        nargs="+",
    )
    parser.add_argument("--reset", action="store_true", default=False)
    parser.add_argument(
        "--skip_build",
        action="store_true",
        default=False,
        help="Skip kernel rebuild, assuming no changes have been made.",
    )
    args = parser.parse_args()

    if args.name == "all":
        for bug in bugs:
            main(bug=bug, run=args.run, reset=args.reset, skip_build=args.skip_build)
    else:
        bug = next((bug for bug in bugs if bug.driver.name == args.name), None)
        if not bug:
            raise ValueError(f"Bug '{args.name}' not found.")
        main(bug=bug, run=args.run, reset=args.reset, skip_build=args.skip_build)
