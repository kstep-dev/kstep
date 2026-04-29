#!/usr/bin/env -S uv run --script

import argparse
import logging
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List

from checkout_linux import Linux, checkout_linux
from run import Driver, make_kstep, make_linux, run_qemu
from scripts import (
    LINUX_ROOT_DIR,
    PROJ_DIR,
    RESULTS_DIR,
    REPRODUCE_LOG,
    set_command_log_path,
    system,
)


@dataclass(frozen=True)
class Bug:
    name: str
    # Commit-based: buggy = fix~1, fixed = fix
    fix: str | None = None
    # Patch-based: buggy = version, fixed = version + patch
    version: str | None = None
    patch: str | None = None
    # Extra kernel config fragment to merge
    config: Path | None = None
    # Driver options
    num_cpus: int = 2
    mem_mb: int = 512
    plot_format: str | None = None

    @property
    def driver(self) -> Driver:
        return Driver(name=self.name, num_cpus=self.num_cpus, mem_mb=self.mem_mb)

    @property
    def linux(self) -> list[Linux]:
        if self.fix:
            return [
                Linux(name="buggy", version=f"{self.fix}~1", config=self.config),
                Linux(name="fixed", version=self.fix, config=self.config),
            ]
        elif self.version and self.patch:
            patch_path = LINUX_ROOT_DIR / self.patch
            return [
                Linux(name="buggy", version=self.version, config=self.config),
                Linux(
                    name="fixed",
                    version=self.version,
                    patch=patch_path,
                    config=self.config,
                ),
            ]
        else:
            raise ValueError(
                f"Bug '{self.name}': specify either 'fix' or 'version'+'patch'"
            )


# fmt: off
BUGS = [
    Bug("sync_wakeup", version="v6.14", patch="sync_wakeup.patch", num_cpus=3, plot_format="curr_task"),
    Bug("vruntime_overflow", fix="bbce3de72be56e4b5f68924b7da9630cc89aa1a8", plot_format="curr_task"),
    Bug("freeze", fix="cd9626e9ebc77edec33023fe95dab4b04ffc819d", plot_format="curr_task"),
    Bug("extra_balance", fix="6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3", num_cpus=5, plot_format="lb_nr_running"),
    Bug("util_avg", fix="17e3e88ed0b6318fde0d1c14df1a804711cab1b5", plot_format="val"),
    Bug("long_balance", fix="2feab2492deb2f14f9675dd6388e9e2bf669c27a", num_cpus=3, mem_mb=4096, plot_format="rebalance"),
    Bug("lag_vruntime", fix="5068d84054b766efe7c6202fc71b2350d1c326f1", plot_format="min_vruntime"),
    Bug("rt_runtime_toggle", fix="9b58e976b3b391c0cf02e038d53dd0478ed3013c", plot_format="curr_task"),
    Bug("uclamp_inversion", fix="0213b7083e81f4acd69db32cb72eb4e5f220329a", plot_format="val"),
    Bug("h_nr_runnable", fix="3429dd57f0deb1a602c2624a1dd7c4c11b6c4734", plot_format="val"),
    Bug("vlag_overflow", fix="1560d1f6eb6b398bddd80c16676776c0325fe5fe", num_cpus=3),
    Bug("throttled_limbo_list", fix="956dfda6a70885f18c0f8236a461aa2bc4f556ad", num_cpus=3),
    Bug("over_schedule", fix="d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1"),
    Bug("slice_update", fix="2f2fc17bab0011430ceb6f2dc1959e7d1f981444"),
    Bug("avg_vruntime_ceil", fix="650cad561cce04b62a8c8e0446b685ef171bc3bb"),
    Bug("min_deadline", fix="8dafa9d0eb1a1550a0f4d462db9354161bc51e0c"),
    Bug("zero_vruntime", fix="b3d99f43c72b56cf7a104a364e7fb34b0702828b"),
]
BUGS_EXTRA = [
    Bug("even_idle_cpu", version="v6.17", patch="even_idle_cpu.patch", num_cpus=5, plot_format="lb_nr_running"),
    
    Bug(
        "local_group_imbalance",
        version="c369299895a591d96745d6492d4888259b004a9e",
        patch="fix_local_group_imbalanced.patch",
        num_cpus=5,
        plot_format="lb_nr_running",
    ),

    Bug(
        "util_avg_jump",
        version="c369299895a591d96745d6492d4888259b004a9e",
        patch="fix_util_avg_jump.patch",
        num_cpus=2,
        plot_format="val",
    )
]

@dataclass
class ProgressState:
    total_bugs: int
    run: List[str]
    bug_name: str = ""
    bug_index: int = 0
    step_index: int = 0

    @property
    def kernel_runs(self) -> list[str]:
        return [name for name in self.run if name != "plot"]

    @property
    def has_plot(self) -> bool:
        return "plot" in self.run

    @property
    def total_steps(self) -> int:
        return len(self.kernel_runs) * 5 + int(self.has_plot)

    def start_bug(self, bug_name: str):
        self.print("-" * 80)
        self.bug_name = bug_name
        self.bug_index += 1
        self.step_index = 1
        self.print_bug_header()

    def print(self, message: str):
        print(message, flush=True)

    def print_bug_header(self):
        variants = ", ".join(self.run)
        self.print(f"[{self.bug_index}/{self.total_bugs}] {self.bug_name}")
        self.print(f"[{self.bug_index}/{self.total_bugs}] plan: {variants}")

    def print_bug_step(self, message: str):
        self.print(
            f"[{self.bug_index}/{self.total_bugs}] "
            f"{self.bug_name} [{self.step_index}/{self.total_steps}] {message}"
        )
        self.step_index += 1

def plot_data(python_script: str, driver: str, progress: ProgressState):
    progress.print_bug_step(f"plot results to results/repro_{driver}/plot.png")
    system(f"{PROJ_DIR}/scripts/plot_{python_script}.py {driver}")


def reproduce(linux: Linux, driver: Driver, skip_build: bool, progress: ProgressState):
    linux_name = f"{driver.name}_{linux.name}"

    progress.print_bug_step(f"{linux.name}: check out Linux {linux.version}")
    checkout_linux(
        linux.version, linux_name=linux_name, patch=linux.patch, tarball=True
    )

    progress.print_bug_step(f"{linux.name}: compile Linux")
    if not skip_build:
        make_linux(linux_name=linux_name)

    progress.print_bug_step(f"{linux.name}: build kSTEP")
    make_kstep(linux_name=linux_name)

    progress.print_bug_step(f"{linux.name}: run QEMU")
    log_file = RESULTS_DIR / f"repro_{driver.name}" / f"{linux.name}.log"
    run_qemu(linux_name=linux_name, driver=driver, log_file=log_file, quiet=True)
    
    progress.print_bug_step(f"{linux.name}: done, log={log_file}")


def main(bug: Bug, skip_build: bool, progress: ProgressState):
    linux_map = {linux.name: linux for linux in bug.linux}
    result_dir = RESULTS_DIR / f"repro_{bug.driver.name}" 
    result_dir.mkdir(exist_ok=True)
    progress.start_bug(bug_name=bug.driver.name)

    for r in progress.kernel_runs:
        linux = linux_map.get(r, Linux(name=r, version=r))
        reproduce(linux, bug.driver, skip_build, progress)

    if progress.has_plot:
        if bug.plot_format:
            plot_data(bug.plot_format, bug.driver.name, progress)
        else:
            print(f"Plot format not specified for bug '{bug.driver.name}'")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "name",
        type=str,
        default="all",
        choices=["all", "ae", *[bug.driver.name for bug in BUGS + BUGS_EXTRA]],
        help="The name of the bug to reproduce, or 'all' to reproduce all BUGS.",
    )
    parser.add_argument(
        "--run",
        type=str,
        default=["buggy", "fixed", "plot"],
        nargs="+",
    )
    parser.add_argument(
        "--skip_build",
        action="store_true",
        default=False,
        help="Skip kernel rebuild, assuming no changes have been made.",
    )
    args = parser.parse_args()

    logging.getLogger().setLevel(logging.WARNING)
    REPRODUCE_LOG.write_text("", encoding="utf-8")
    set_command_log_path(REPRODUCE_LOG)

    if args.name == "all":
        selected_bugs = BUGS
    elif args.name == "ae":
        selected_bugs = BUGS[: 7] + BUGS_EXTRA[: 1]
    else:
        bug = next((bug for bug in BUGS + BUGS_EXTRA if bug.driver.name == args.name), None)
        if not bug:
            raise ValueError(f"Bug '{args.name}' not found.")
        selected_bugs = [bug]

    total_bugs = len(selected_bugs)
    progress = ProgressState(total_bugs=total_bugs, run=args.run)
    print(f"running {total_bugs} bug(s); detailed logs: {REPRODUCE_LOG}")
    for bug in selected_bugs:
        main(
            bug=bug,
            skip_build=args.skip_build,
            progress=progress,
        )
