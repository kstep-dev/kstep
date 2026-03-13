# NUMA: NULL pointer dereference in __migrate_swap_task on task exit race

**Commit:** `db6cc3f4ac2e6cdc898fc9cbc8b32ae1bf56bdad`
**Affected files:** kernel/sched/core.c, kernel/sched/debug.c
**Fixed in:** v6.16-rc6
**Buggy since:** v6.16-rc1 (introduced by commit ad6b26b6a0a7 "sched/numa: add statistics of numa balance task")

## Bug Description

Commit `ad6b26b6a0a7` added per-memcg and per-task NUMA balance statistics to track task migration and task swapping events triggered by the kernel's NUMA balancing subsystem. Specifically, it added calls to `count_memcg_event_mm(p->mm, NUMA_TASK_SWAP)` in `__migrate_swap_task()` and `count_memcg_event_mm(p->mm, NUMA_TASK_MIGRATE)` in `migrate_task_to()`. These calls dereference `p->mm` to locate the memory cgroup associated with the task's address space.

However, these statistics calls were inserted without proper synchronization against concurrent task exit. When NUMA balancing selects a task as a swap candidate (via `task_numa_compare()`), there is a window between the selection and the actual execution of the swap (via `migrate_swap_stop()`) during which the selected task can exit on another CPU. When a task exits, `exit_mm()` sets `p->mm = NULL`. If the swap proceeds after this point, `count_memcg_event_mm()` receives a NULL pointer and dereferences it, causing a kernel panic.

The fix reverts the entire statistics commit rather than adding locking, because the maintainers agreed that adding `task_lock()` and `PF_EXITING` checks purely for optional statistics counters was not worthwhile. The tracepoint mechanism already provides equivalent observability without the synchronization hazard.

## Root Cause

The root cause is a classic time-of-check-to-time-of-use (TOCTOU) race between NUMA balancing task selection and task exit. The NUMA balancing path works as follows:

1. `task_numa_migrate()` calls `task_numa_find_cpu()` to find the best CPU for the current task.
2. `task_numa_find_cpu()` iterates over candidate CPUs and calls `task_numa_compare()` for each, which may select a remote task `p` as `env->best_task` for swapping.
3. After selection, `migrate_swap(cur, p)` is called, which uses `stop_two_cpus()` to execute `migrate_swap_stop()` on the relevant CPUs.
4. `migrate_swap_stop()` calls `__migrate_swap_task()` for both the source and destination tasks.

The buggy code added these lines at the beginning of `__migrate_swap_task()`:

```c
__schedstat_inc(p->stats.numa_task_swapped);
count_vm_numa_event(NUMA_TASK_SWAP);
count_memcg_event_mm(p->mm, NUMA_TASK_SWAP);
```

And similarly in `migrate_task_to()`:

```c
__schedstat_inc(p->stats.numa_task_migrated);
count_vm_numa_event(NUMA_TASK_MIGRATE);
count_memcg_event_mm(p->mm, NUMA_TASK_MIGRATE);
```

The `count_memcg_event_mm()` function takes an `mm_struct *` as its first argument and dereferences it to find the associated `mem_cgroup`. When `p->mm` is NULL (because the task has exited and `exit_mm()` has cleared it), this results in a NULL pointer dereference.

The race window exists because `task_numa_compare()` takes a reference on the candidate task (via `get_task_struct()`), which prevents the `task_struct` from being freed but does NOT prevent the task from exiting and having its `mm_struct` cleared. The reference only keeps the `task_struct` memory valid — it does not keep the task alive or its `mm` pointer valid. Between the moment `env->best_task = p` is set and the moment `__migrate_swap_task()` runs inside the CPU stopper, `exit_mm()` can execute on another CPU setting `p->mm = NULL`.

The proper fix would require holding `task_lock()` around the `p->mm` access and checking `PF_EXITING` in `p->flags`, but since this is only for statistics (not functional correctness), the overhead was deemed unacceptable and the feature was reverted.

## Consequence

The consequence is a kernel panic via NULL pointer dereference. The crash manifests as:

```
BUG: kernel NULL pointer dereference, address: 00000000000004c8
RIP: 0010:__migrate_swap_task+0x2f/0x170
```

The faulting address `0x4c8` corresponds to the offset within the `mm_struct` that `count_memcg_event_mm()` attempts to read (specifically, the `owner` field or the memcg pointer within `mm_struct`). The crash occurs in the context of a `migration/*` kernel thread executing `migrate_swap_stop()` via the CPU stopper mechanism (`multi_cpu_stop`). The call trace shows:

```
migrate_swap_stop+0xe8/0x190
multi_cpu_stop+0xf3/0x130
cpu_stopper_thread+0x97/0x140
smpboot_thread_fn+0xf3/0x220
kthread+0xfc/0x240
```

This was reported as a reproducible crash on multiple systems — dual-socket AMD EPYC (Milan and Genoa) and Intel platforms — while running CPU-intensive workloads such as Linpack and stress-ng. The crash typically occurs after several hours of performance testing on NUMA systems where NUMA balancing is active and tasks are being migrated/swapped frequently. It affects Fedora rawhide (ELN) kernels based on 6.16-rc1 and 6.16-rc2, as well as linux-next trees starting from next-20250616.

## Fix Summary

The fix is a complete revert of commit `ad6b26b6a0a7`. It removes all the NUMA task migration/swap statistics that were added:

1. **kernel/sched/core.c**: Removes `__schedstat_inc(p->stats.numa_task_swapped)`, `count_vm_numa_event(NUMA_TASK_SWAP)`, and `count_memcg_event_mm(p->mm, NUMA_TASK_SWAP)` from `__migrate_swap_task()`. Removes `__schedstat_inc(p->stats.numa_task_migrated)`, `count_vm_numa_event(NUMA_TASK_MIGRATE)`, and `count_memcg_event_mm(p->mm, NUMA_TASK_MIGRATE)` from `migrate_task_to()`. A `/* TODO: This is not properly updating schedstats */` comment is left in `migrate_task_to()`.

2. **include/linux/sched.h**: Removes the `numa_task_migrated` and `numa_task_swapped` fields from `struct sched_statistics`.

3. **include/linux/vm_event_item.h**: Removes the `NUMA_TASK_MIGRATE` and `NUMA_TASK_SWAP` enum values from `enum vm_event_item`.

4. **kernel/sched/debug.c**: Removes the `P_SCHEDSTAT(numa_task_migrated)` and `P_SCHEDSTAT(numa_task_swapped)` display lines from `proc_sched_show_task()`.

5. **mm/memcontrol.c**: Removes `NUMA_TASK_MIGRATE` and `NUMA_TASK_SWAP` from the `memcg_vm_event_stat[]` array.

6. **mm/vmstat.c**: Removes the `"numa_task_migrated"` and `"numa_task_swapped"` string entries from `vmstat_text[]`.

7. **Documentation/admin-guide/cgroup-v2.rst**: Removes the documentation entries for `numa_task_migrated` and `numa_task_swapped` from the memory.stat description.

The revert is correct and complete because it eliminates the only code path that dereferences `p->mm` without proper locking in the NUMA migration path. The existing tracepoints (`trace_sched_move_numa`, `trace_sched_swap_numa`) already provide equivalent observability for NUMA task migration events.

## Triggering Conditions

The following conditions are required to trigger this bug:

- **NUMA topology**: The system must have multiple NUMA nodes (at least 2). The bug was reported on dual-socket AMD EPYC systems (Milan and Genoa) and Intel multi-socket systems.

- **NUMA balancing enabled**: `CONFIG_NUMA_BALANCING=y` must be set and the runtime knob `/proc/sys/kernel/numa_balancing` must be enabled (it is on by default in most distributions).

- **SCHEDSTATS enabled** (partial): The `count_memcg_event_mm()` call does not require SCHEDSTATS, but the `__schedstat_inc()` calls do. The NULL dereference in `count_memcg_event_mm()` occurs regardless of SCHEDSTATS configuration.

- **Memory cgroups**: `CONFIG_MEMCG=y` must be set for `count_memcg_event_mm()` to be compiled in (though it may be a no-op inline if memcg is disabled at runtime).

- **CPU-intensive workload with task churn**: The workload must create enough memory access pressure across NUMA nodes to trigger NUMA balancing scans and task swaps. Additionally, tasks must be exiting (or being created and destroyed) frequently enough that the race window between task selection and swap execution is hit. Benchmarks like Linpack and stress-ng on large NUMA systems reproduce this after several hours.

- **Concurrency**: The race requires at least two CPUs: one executing the NUMA balancing migration path (`task_numa_migrate` → `migrate_swap_stop` → `__migrate_swap_task`), and another where the selected swap candidate task is exiting (`do_exit` → `exit_mm`). The more CPUs and the higher the task churn, the more likely the race window is hit.

- **Task swap path specifically**: The `__migrate_swap_task()` path (not just `migrate_task_to()`) is more commonly triggered because NUMA balancing frequently swaps tasks between nodes rather than simply migrating them.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. It is classified as unplanned for the following fundamental reasons:

### 1. NUMA balancing requires real userspace memory access patterns

NUMA balancing works by periodically scanning a task's page table entries (via `task_numa_work()`) and changing present PTEs to NUMA hinting entries. When the task subsequently accesses those pages, it triggers NUMA hinting page faults (`do_numa_page()`), which the kernel uses to determine the task's preferred NUMA node and decide whether to migrate pages or tasks. kSTEP tasks are kernel-controlled entities that do not perform real userspace memory accesses. They cannot generate NUMA hinting faults, and therefore the NUMA balancing subsystem will never select them for migration or swapping.

### 2. kSTEP tasks lack real mm_struct

kSTEP creates tasks via `kstep_task_create()` and `kstep_kthread_create()`, which produce kernel threads. Kernel threads borrow the `init_mm` or have `p->mm = NULL` — they do not have their own `mm_struct` with user-space virtual memory mappings. NUMA balancing only operates on tasks with a valid `p->mm` (i.e., userspace processes), and `task_numa_work()` explicitly checks for `p->mm` before proceeding. Without a real `mm_struct`, kSTEP tasks will never enter the NUMA balancing code path.

### 3. The race condition requires real task lifecycle management

The bug is triggered by a race between NUMA task swap selection and `exit_mm()` during task exit. Reproducing this requires the ability to have a real userspace process exit (calling `do_exit()` → `exit_signals()` → `exit_mm()`) at precisely the right moment — after being selected as a NUMA swap candidate but before the swap is executed. kSTEP's task management API (`kstep_task_pause`, `kstep_task_block`, etc.) controls task scheduling state but does not simulate real process exit with `exit_mm()`.

### 4. Cannot simulate real NUMA memory latencies

Even if kSTEP could set up NUMA topology structure via `kstep_topo_set_node()`, it cannot simulate the actual NUMA memory access latencies that drive NUMA balancing decisions. The kernel's NUMA balancing logic uses fault statistics and access patterns to determine task placement — this requires real memory being accessed on real (or at least emulated) NUMA nodes with different access latencies.

### 5. What would need to change in kSTEP

To support reproduction of this class of bugs, kSTEP would need:
- **Real userspace process creation**: The ability to fork actual userspace processes with their own `mm_struct` and virtual memory mappings, not just kernel threads.
- **Memory allocation on specific NUMA nodes**: APIs to allocate memory on particular NUMA nodes so that NUMA balancing has reason to migrate tasks.
- **Simulated NUMA memory latency**: QEMU would need to be configured with actual NUMA memory latency differentiation (QEMU supports `-numa` options but kSTEP doesn't expose this).
- **Process exit control**: The ability to cause a userspace process to exit at a precise moment during the NUMA balancing scan/swap window.
- **NUMA fault injection**: An alternative approach would be to add a mechanism to directly inject NUMA hinting faults or manually trigger `task_numa_migrate()` for a specific task, but this would require significant kernel-side instrumentation.

These are fundamental architectural changes — not minor API additions — because they require kSTEP to manage real userspace processes with real memory mappings, which is outside its current kernel-module-based architecture.

### 6. Alternative reproduction methods

Outside kSTEP, this bug can be reproduced on a real multi-socket NUMA system by:
1. Running a CPU and memory-intensive multi-threaded workload (stress-ng, Linpack) that creates memory pressure across NUMA nodes.
2. Ensuring NUMA balancing is enabled (`echo 1 > /proc/sys/kernel/numa_balancing`).
3. Running a concurrent workload that creates and destroys processes rapidly to increase task churn and the probability of hitting the race window.
4. Waiting several hours — reporters indicate the crash typically occurs after extended runtime (not immediately).
5. The kernel version must be between v6.16-rc1 (which includes `ad6b26b6a0a7`) and before the revert in v6.16-rc6.
