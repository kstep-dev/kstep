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
        driver=Driver(name="sync_wakeup", num_cpus=3),
        linux=[
            Linux(name="buggy", version="v6.14"),
            Linux(
                name="fixed",
                version="v6.14",
                patch=LINUX_ROOT_DIR / "sync_wakeup.patch",
            ),
        ],
        plot_format="curr_task",
    ),
    Bug(
        driver=Driver(name="freeze", num_cpus=2),
        linux=[
            Linux(name="buggy", version="cd9626e9ebc77edec33023fe95dab4b04ffc819d~1"),
            Linux(name="fixed", version="cd9626e9ebc77edec33023fe95dab4b04ffc819d"),
        ],
        plot_format="curr_task",
    ),
    Bug(
        driver=Driver(name="vruntime_overflow", num_cpus=2),
        linux=[
            Linux(name="buggy", version="bbce3de72be56e4b5f68924b7da9630cc89aa1a8~1"),
            Linux(name="fixed", version="bbce3de72be56e4b5f68924b7da9630cc89aa1a8"),
        ],
        plot_format="curr_task",
    ),
    Bug(
        driver=Driver(name="long_balance", num_cpus=3, mem_mb=4096),
        linux=[
            Linux(name="buggy", version="2feab2492deb2f14f9675dd6388e9e2bf669c27a~1"),
            Linux(name="fixed", version="2feab2492deb2f14f9675dd6388e9e2bf669c27a"),
        ],
        plot_format="rebalance",
    ),
    Bug(
        driver=Driver(name="util_avg", num_cpus=2),
        linux=[
            Linux(name="buggy", version="17e3e88ed0b6318fde0d1c14df1a804711cab1b5~1"),
            Linux(name="fixed", version="17e3e88ed0b6318fde0d1c14df1a804711cab1b5"),
        ],
        plot_format="val",
    ),
    Bug(
        driver=Driver(name="lag_vruntime", num_cpus=2),
        linux=[
            Linux(name="buggy", version="5068d84054b766efe7c6202fc71b2350d1c326f1~1"),
            Linux(name="fixed", version="5068d84054b766efe7c6202fc71b2350d1c326f1"),
        ],
        plot_format="min_vruntime",
    ),
    Bug(
        driver=Driver(name="even_idle_cpu", num_cpus=5),
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
        driver=Driver(name="extra_balance", num_cpus=5),
        linux=[
            Linux(name="buggy", version="6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3~1"),
            Linux(name="fixed", version="6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3"),
        ],
        plot_format="lb_nr_running",
    ),
    Bug(
        driver=Driver(name="rt_runtime_toggle", num_cpus=2),
        linux=[
            Linux(name="buggy", version="9b58e976b3b391c0cf02e038d53dd0478ed3013c~1"),
            Linux(name="fixed", version="9b58e976b3b391c0cf02e038d53dd0478ed3013c"),
        ],
        plot_format="curr_task",
    ),
    Bug(
        driver=Driver(name="uclamp_inversion", num_cpus=2),
        linux=[
            Linux(name="buggy", version="0213b7083e81f4acd69db32cb72eb4e5f220329a~1"),
            Linux(name="fixed", version="0213b7083e81f4acd69db32cb72eb4e5f220329a"),
        ],
        plot_format="val",
    ),
    Bug(
        driver=Driver(name="h_nr_runnable", num_cpus=2),
        linux=[
            Linux(name="buggy", version="3429dd57f0deb1a602c2624a1dd7c4c11b6c4734~1"),
            Linux(name="fixed", version="3429dd57f0deb1a602c2624a1dd7c4c11b6c4734"),
        ],
        plot_format="val",
    ),
    Bug(
        driver=Driver(name="vlag_overflow", num_cpus=3),
        linux=[
            Linux(name="buggy", version="1560d1f6eb6b398bddd80c16676776c0325fe5fe~1"),
            Linux(name="fixed", version="1560d1f6eb6b398bddd80c16676776c0325fe5fe"),
        ],
    ),
    Bug(
        driver=Driver(name="throttled_limbo_list", num_cpus=3),
        linux=[
            Linux(name="buggy", version="956dfda6a70885f18c0f8236a461aa2bc4f556ad~1"),
            Linux(name="fixed", version="956dfda6a70885f18c0f8236a461aa2bc4f556ad"),
        ],
    ),
    Bug(
        driver=Driver(name="over_schedule", num_cpus=2),
        linux=[
            Linux(name="buggy", version="d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1~1"),
            Linux(name="fixed", version="d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1"),
        ],
    ),
    Bug(
        driver=Driver(name="slice_update", num_cpus=2),
        linux=[
            Linux(name="buggy", version="2f2fc17bab0011430ceb6f2dc1959e7d1f981444~1"),
            Linux(name="fixed", version="2f2fc17bab0011430ceb6f2dc1959e7d1f981444"),
        ],
    ),
    Bug(
        driver=Driver(name="avg_vruntime_ceil", num_cpus=2),
        linux=[
            Linux(name="buggy", version="650cad561cce04b62a8c8e0446b685ef171bc3bb~1"),
            Linux(name="fixed", version="650cad561cce04b62a8c8e0446b685ef171bc3bb"),
        ],
    ),
    Bug(
        driver=Driver(name="min_deadline", num_cpus=2),
        linux=[
            Linux(name="buggy", version="8dafa9d0eb1a1550a0f4d462db9354161bc51e0c~1"),
            Linux(name="fixed", version="8dafa9d0eb1a1550a0f4d462db9354161bc51e0c"),
        ],
    ),
    Bug(
        driver=Driver(name="rq_qos_wakeup"),
        linux=[
            Linux(name="buggy", version="11c7aa0ddea8611007768d3e6b58d45dc60a19e1~1"),
            Linux(name="fixed", version="11c7aa0ddea8611007768d3e6b58d45dc60a19e1"),
        ],
    ),
    Bug(
        driver=Driver(name="balance_push_splice", num_cpus=2),
        linux=[
            Linux(name="buggy", version="04193d590b390ec7a0592630f46d559ec6564ba1~1"),
            Linux(name="fixed", version="04193d590b390ec7a0592630f46d559ec6564ba1"),
        ],
    ),
    Bug(
        driver=Driver(name="migrate_swap_hotplug", num_cpus=2),
        linux=[
            Linux(name="buggy", version="009836b4fa52f92cba33618e773b1094affa8cd2~1"),
            Linux(name="fixed", version="009836b4fa52f92cba33618e773b1094affa8cd2"),
        ],
    ),
    Bug(
        driver=Driver(name="migrate_overflow"),
        linux=[
            Linux(
                name="buggy",
                version="0ec8d5aed4d14055aab4e2746def33f8b0d409c3~1",
            ),
            Linux(
                name="fixed",
                version="0ec8d5aed4d14055aab4e2746def33f8b0d409c3",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="sched_change_assert", num_cpus=2),
        linux=[
            Linux(
                name="buggy",
                version="1862d8e264def8425d682f1177e22f9fe7d947ea~1",
            ),
            Linux(
                name="fixed",
                version="1862d8e264def8425d682f1177e22f9fe7d947ea",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="hrtick_reprogram", num_cpus=2),
        linux=[
            Linux(
                name="buggy",
                version="156ec6f42b8d300dbbf382738ff35c8bad8f4c3a~1",
            ),
            Linux(
                name="fixed",
                version="156ec6f42b8d300dbbf382738ff35c8bad8f4c3a",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="migration_cpu_stop_warn", num_cpus=3),
        linux=[
            Linux(
                name="buggy",
                version="1293771e4353c148d5f6908fb32d1c1cfd653e47~1",
            ),
            Linux(
                name="fixed",
                version="1293771e4353c148d5f6908fb32d1c1cfd653e47",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="reject_affinity", num_cpus=2),
        linux=[
            Linux(name="buggy", version="234a503e670b~1"),
            Linux(name="fixed", version="234a503e670b"),
        ],
    ),
    Bug(
        driver=Driver(name="mm_cid_perf", num_cpus=3),
        linux=[
            Linux(
                name="buggy",
                version="223baf9d17f25e2608dbdff7232c095c1e612268~1",
            ),
            Linux(
                name="fixed",
                version="223baf9d17f25e2608dbdff7232c095c1e612268",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="charge_percpu_cpuusage", num_cpus=3),
        linux=[
            Linux(name="buggy", version="248cc9993d1c~1"),
            Linux(name="fixed", version="248cc9993d1c"),
        ],
    ),
    Bug(
        driver=Driver(name="sched_debug_output", num_cpus=2),
        linux=[
            Linux(name="buggy", version="2cab4bd024d2~1"),
            Linux(name="fixed", version="2cab4bd024d2"),
        ],
    ),
    Bug(
        driver=Driver(name="nr_running_wakelist", num_cpus=2),
        linux=[
            Linux(name="buggy", version="28156108fecb~1"),
            Linux(name="fixed", version="28156108fecb"),
        ],
    ),
    Bug(
        driver=Driver(name="pf_kthread_race", num_cpus=2),
        linux=[
            Linux(name="buggy", version="3a7956e25e1d~1"),
            Linux(name="fixed", version="3a7956e25e1d"),
        ],
    ),
    Bug(
        driver=Driver(name="preempt_str", num_cpus=2),
        linux=[
            Linux(
                name="buggy",
                version="3ebb1b652239~1",
                config=LINUX_ROOT_DIR / "preempt_str.config",
            ),
            Linux(
                name="fixed",
                version="3ebb1b652239",
                config=LINUX_ROOT_DIR / "preempt_str.config",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="cpus_share_cache", num_cpus=3),
        linux=[
            Linux(name="buggy", version="42dc938a590c~1"),
            Linux(name="fixed", version="42dc938a590c"),
        ],
    ),
    Bug(
        driver=Driver(name="fix_balance_callback", num_cpus=2),
        linux=[
            Linux(name="buggy", version="565790d28b1e~1"),
            Linux(name="fixed", version="565790d28b1e"),
        ],
    ),
    Bug(
        driver=Driver(name="forceidle_balancing", num_cpus=2),
        linux=[
            Linux(
                name="buggy",
                version="5b6547ed97f4~1",
                config=LINUX_ROOT_DIR / "forceidle_balancing.config",
                patch=LINUX_ROOT_DIR / "forceidle_balancing.patch",
            ),
            Linux(
                name="fixed",
                version="5b6547ed97f4",
                config=LINUX_ROOT_DIR / "forceidle_balancing.config",
                patch=LINUX_ROOT_DIR / "forceidle_balancing.patch",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="wakeup_next_class", num_cpus=3),
        linux=[
            Linux(name="buggy", version="5324953c06bd~1"),
            Linux(name="fixed", version="5324953c06bd"),
        ],
    ),
    Bug(
        driver=Driver(name="exit_mm_membarrier", num_cpus=2),
        linux=[
            Linux(name="buggy", version="5bc78502322a~1"),
            Linux(name="fixed", version="5bc78502322a"),
        ],
    ),
    Bug(
        driver=Driver(name="task_state_cmp", num_cpus=2),
        linux=[
            Linux(name="buggy", version="5aec788aeb8e~1"),
            Linux(name="fixed", version="5aec788aeb8e"),
        ],
    ),
    Bug(
        driver=Driver(name="yield_to_race", num_cpus=4),
        linux=[
            Linux(name="buggy", version="5d808c78d972~1"),
            Linux(name="fixed", version="5d808c78d972"),
        ],
    ),
    Bug(
        driver=Driver(name="rqcf_act_skip_leak", num_cpus=3),
        linux=[
            Linux(name="buggy", version="5ebde09d9170~1"),
            Linux(name="fixed", version="5ebde09d9170"),
        ],
    ),
    Bug(
        driver=Driver(name="uclamp_oob", num_cpus=2),
        linux=[
            Linux(
                name="buggy",
                version="6d2f8909a5fa~1",
                config=LINUX_ROOT_DIR / "uclamp_oob.config",
            ),
            Linux(
                name="fixed",
                version="6d2f8909a5fa",
                config=LINUX_ROOT_DIR / "uclamp_oob.config",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="null_term_scaling", num_cpus=2),
        linux=[
            Linux(name="buggy", version="703066188f63~1"),
            Linux(name="fixed", version="703066188f63"),
        ],
    ),
    Bug(
        driver=Driver(name="mmcid_compact", num_cpus=3),
        linux=[
            Linux(name="buggy", version="77d7dc8bef48~1"),
            Linux(name="fixed", version="77d7dc8bef48"),
        ],
    ),
    Bug(
        driver=Driver(name="setaffinity_warn", num_cpus=4),
        linux=[
            Linux(name="buggy", version="70ee7947a290~1"),
            Linux(name="fixed", version="70ee7947a290"),
        ],
    ),
    Bug(
        driver=Driver(name="sched_move", num_cpus=2),
        linux=[
            Linux(name="buggy", version="76f970ce51c8~1"),
            Linux(name="fixed", version="76f970ce51c8"),
        ],
    ),
    Bug(
        driver=Driver(name="forceidle_starvation", num_cpus=3),
        linux=[
            Linux(name="buggy", version="8039e96fcc1d~1"),
            Linux(name="fixed", version="8039e96fcc1d"),
        ],
    ),
    Bug(
        driver=Driver(name="freq_tick"),
        linux=[
            Linux(
                name="buggy",
                version="7fb3ff22ad87~1",
                config=LINUX_ROOT_DIR / "freq_tick.config",
                patch=LINUX_ROOT_DIR / "freq_tick.patch",
            ),
            Linux(
                name="fixed",
                version="7fb3ff22ad87",
                config=LINUX_ROOT_DIR / "freq_tick.config",
                patch=LINUX_ROOT_DIR / "freq_tick.patch",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="dup_user_cpus_uaf", num_cpus=2),
        linux=[
            Linux(name="buggy", version="87ca4f9efbd7~1"),
            Linux(name="fixed", version="87ca4f9efbd7"),
        ],
    ),
    Bug(
        driver=Driver(name="cond_resched_irq", num_cpus=2),
        linux=[
            Linux(name="buggy", version="82c387ef7568~1"),
            Linux(name="fixed", version="82c387ef7568"),
        ],
    ),
    Bug(
        driver=Driver(name="resched_warn", num_cpus=2),
        linux=[
            Linux(name="buggy", version="8061b9f5e111~1"),
            Linux(name="fixed", version="8061b9f5e111"),
        ],
    ),
    Bug(
        driver=Driver(name="freq_tick", num_cpus=2),
        linux=[
            Linux(name="buggy", version="7fb3ff22ad87~1"),
            Linux(name="fixed", version="7fb3ff22ad87"),
        ],
    ),
    Bug(
        driver=Driver(name="migration_cpu_stop_requeue", num_cpus=3),
        linux=[
            Linux(name="buggy", version="8a6edb5257e2~1"),
            Linux(name="fixed", version="8a6edb5257e2"),
        ],
    ),
    Bug(
        driver=Driver(name="task_call_func_race", num_cpus=3),
        linux=[
            Linux(name="buggy", version="91dabf33ae5d~1"),
            Linux(name="fixed", version="91dabf33ae5d"),
        ],
    ),
    Bug(
        driver=Driver(name="core_enqueue", num_cpus=3),
        linux=[
            Linux(name="buggy", version="91caa5ae2424~1"),
            Linux(name="fixed", version="91caa5ae2424"),
        ],
    ),
    Bug(
        driver=Driver(name="migrate_disable_lock", num_cpus=2),
        linux=[
            Linux(name="buggy", version="942b8db96500~1"),
            Linux(name="fixed", version="942b8db96500"),
        ],
    ),
    Bug(
        driver=Driver(name="sched_switch_prev_state", num_cpus=2),
        linux=[
            Linux(name="buggy", version="8feb053d5319~1"),
            Linux(name="fixed", version="8feb053d5319"),
        ],
    ),
    Bug(
        driver=Driver(
            name="sched_debug_flags_corrupt",
            kernel_params=("slub_debug=FZ",),
        ),
        linux=[
            Linux(
                name="buggy",
                version="8d4d9c7b4333~1",
                patch=LINUX_ROOT_DIR / "objtool_symtab.patch",
            ),
            Linux(
                name="fixed",
                version="8d4d9c7b4333",
                patch=LINUX_ROOT_DIR / "objtool_symtab.patch",
            ),
        ],
    ),
    Bug(
        driver=Driver(name="double_clock"),
        linux=[
            Linux(name="buggy", version="96500560f0c73c71bca1b27536c6254fa0e8ce37~1"),
            Linux(name="fixed", version="96500560f0c73c71bca1b27536c6254fa0e8ce37"),
        ],
    ),
    Bug(
        driver=Driver(name="migration_underflow"),
        linux=[
            Linux(name="buggy", version="9d0df37797453f168afdb2e6fd0353c73718ae9a~1"),
            Linux(name="fixed", version="9d0df37797453f168afdb2e6fd0353c73718ae9a"),
        ],
    ),
]


def plot_data(python_script: str, driver: str):
    system(f"{PROJ_DIR}/scripts/plot_{python_script}.py {driver}")


def reproduce(linux: Linux, driver: Driver, skip_build: bool):
    linux_name = f"{driver.name}_{linux.name}"
    checkout_linux(
        linux.version, linux_name=linux_name, patch=linux.patch, tarball=True
    )
    if not skip_build:
        if linux.config:
            # Generate default config, merge extra fragment, then build
            system(f"make -C {PROJ_DIR} linux-config LINUX_NAME={linux_name}")
            linux_dir = LINUX_ROOT_DIR / linux_name
            system(
                f"cd {linux_dir} && "
                f"./scripts/kconfig/merge_config.sh -m .config {linux.config} && "
                f"make olddefconfig"
            )
        make_linux(linux_name=linux_name)
    make_kstep(linux_name=linux_name)
    log_file = RESULTS_DIR / f"{linux_name}.log"
    run_qemu(linux_name=linux_name, driver=driver, log_file=log_file)
    print_run_results(linux_name=linux_name)


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
