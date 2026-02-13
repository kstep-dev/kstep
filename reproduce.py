#!/usr/bin/env -S uv run --script

import argparse
from dataclasses import dataclass
from typing import List

from checkout_linux import Linux, checkout_linux
from run import Driver, make_kstep, make_linux, print_run_results, run_qemu
from scripts import LINUX_ROOT_DIR, PROJ_DIR, RESULTS_DIR, system


@dataclass(frozen=True)
class Bug:
    driver: Driver
    linux: List[Linux]
    plot_format: str | None = None


bugs = [
    Bug(
        driver=Driver(name="sync_wakeup", smp="3"),
        linux=[
            Linux(name="buggy", version="v6.14"),
            Linux(
                name="fixed",
                version="v6.14",
                patch=LINUX_ROOT_DIR / "sync_wakeup.patch",
            ),
        ],
        plot_format="cur_task",
    ),
    Bug(
        driver=Driver(name="freeze", smp="2"),
        linux=[
            Linux(name="buggy", version="cd9626e9ebc77edec33023fe95dab4b04ffc819d~1"),
            Linux(name="fixed", version="cd9626e9ebc77edec33023fe95dab4b04ffc819d"),
        ],
        plot_format="cur_task",
    ),
    Bug(
        driver=Driver(name="vruntime_overflow", smp="2"),
        linux=[
            Linux(name="buggy", version="bbce3de72be56e4b5f68924b7da9630cc89aa1a8~1"),
            Linux(name="fixed", version="bbce3de72be56e4b5f68924b7da9630cc89aa1a8"),
        ],
        plot_format="cur_task",
    ),
    Bug(
        driver=Driver(name="long_balance", smp="3", mem_mb=4096),
        linux=[
            Linux(name="buggy", version="2feab2492deb2f14f9675dd6388e9e2bf669c27a~1"),
            Linux(name="fixed", version="2feab2492deb2f14f9675dd6388e9e2bf669c27a"),
        ],
        plot_format="rebalance",
    ),
    Bug(
        driver=Driver(name="util_avg", smp="2"),
        linux=[
            Linux(name="buggy", version="17e3e88ed0b6318fde0d1c14df1a804711cab1b5~1"),
            Linux(name="fixed", version="17e3e88ed0b6318fde0d1c14df1a804711cab1b5"),
        ],
        plot_format="util_avg",
    ),
    Bug(
        driver=Driver(name="lag_vruntime", smp="2"),
        linux=[
            Linux(name="buggy", version="5068d84054b766efe7c6202fc71b2350d1c326f1~1"),
            Linux(name="fixed", version="5068d84054b766efe7c6202fc71b2350d1c326f1"),
        ],
        plot_format="min_vruntime",
    ),
    Bug(
        driver=Driver(name="even_idle_cpu", smp="5"),
        linux=[
            Linux(name="buggy", version="v6.17"),
            Linux(
                name="fixed",
                version="v6.17",
                patch=LINUX_ROOT_DIR / "even_idle_cpu.patch",
            ),
        ],
        plot_format="lb_nr_running",
    ),
    Bug(
        driver=Driver(name="extra_balance", smp="8,sockets=2,cores=2,threads=2"),
        linux=[
            Linux(name="buggy", version="6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3~1"),
            Linux(name="fixed", version="6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3"),
        ],
        plot_format="lb_nr_running",
    ),
    Bug(
        driver=Driver(name="rt_runtime_toggle", smp="2"),
        linux=[
            Linux(name="buggy", version="9b58e976b3b391c0cf02e038d53dd0478ed3013c~1"),
            Linux(name="fixed", version="9b58e976b3b391c0cf02e038d53dd0478ed3013c"),
        ],
        plot_format="cur_task",
    ),
    Bug(
        driver=Driver(name="uclamp_inversion", smp="2"),
        linux=[
            Linux(name="buggy", version="0213b7083e81f4acd69db32cb72eb4e5f220329a~1"),
            Linux(name="fixed", version="0213b7083e81f4acd69db32cb72eb4e5f220329a"),
        ],
        plot_format="util_avg",
    ),
    Bug(
        driver=Driver(name="h_nr_runnable", smp="2"),
        linux=[
            Linux(name="buggy", version="3429dd57f0deb1a602c2624a1dd7c4c11b6c4734~1"),
            Linux(name="fixed", version="3429dd57f0deb1a602c2624a1dd7c4c11b6c4734"),
        ],
        plot_format="util_avg",
    ),
    Bug(
        driver=Driver(name="vlag_overflow", smp="3"),
        linux=[
            Linux(name="buggy", version="1560d1f6eb6b398bddd80c16676776c0325fe5fe~1"),
            Linux(name="fixed", version="1560d1f6eb6b398bddd80c16676776c0325fe5fe"),
        ],
    ),
    Bug(
        driver=Driver(name="throttled_limbo_list", smp="2"),
        linux=[
            Linux(name="buggy", version="956dfda6a70885f18c0f8236a461aa2bc4f556ad~1"),
            Linux(name="fixed", version="956dfda6a70885f18c0f8236a461aa2bc4f556ad"),
        ],
    ),
    Bug(
        driver=Driver(name="over_schedule", smp="2"),
        linux=[
            Linux(name="buggy", version="d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1~1"),
            Linux(name="fixed", version="d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1"),
        ],
    ),
]


def plot_data(python_script: str, driver: str):
    system(f"{PROJ_DIR}/scripts/plot_{python_script}.py {driver}")


def reproduce(linux: Linux, driver: Driver, skip_build: bool):
    linux_dir = LINUX_ROOT_DIR / f"{driver.name}_{linux.name}"
    checkout_linux(linux.version, linux_dir=linux_dir, patch=linux.patch, tarball=True)
    if not skip_build:
        make_linux(linux_dir=linux_dir)
    make_kstep(linux_dir=linux_dir)
    log_file = RESULTS_DIR / f"{driver.name}_{linux.name}.log"
    proc = run_qemu(linux_dir=linux_dir, driver=driver, log_file=log_file)
    return_code = proc.wait()
    print(f"Reproduction returned with code: {return_code}")
    print_run_results()


def main(bug: Bug, run: List[str], skip_build: bool):
    linux_map = {linux.name: linux for linux in bug.linux}
    for r in [r for r in run if r != "plot"]:
        linux = linux_map.get(r, Linux(name=r, version=r))
        reproduce(linux, bug.driver, skip_build)

    if "plot" in run:
        if bug.plot_format:
            plot_data(bug.plot_format, bug.driver.name)
        else:
            print(f"Plot format not specified for bug '{bug.driver.name}'")


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
    parser.add_argument(
        "--skip_build",
        action="store_true",
        default=False,
        help="Skip kernel rebuild, assuming no changes have been made.",
    )
    args = parser.parse_args()

    if args.name == "all":
        for bug in bugs:
            main(bug=bug, run=args.run, skip_build=args.skip_build)
    else:
        bug = next((bug for bug in bugs if bug.driver.name == args.name), None)
        if not bug:
            raise ValueError(f"Bug '{args.name}' not found.")
        main(bug=bug, run=args.run, skip_build=args.skip_build)
