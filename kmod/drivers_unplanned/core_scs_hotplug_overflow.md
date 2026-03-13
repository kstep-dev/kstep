# Core: Shadow Call Stack Overflow on CPU Hotplug

**Commit:** `63acd42c0d4942f74710b11c38602fb14dea7320`
**Affected files:** `kernel/sched/core.c`
**Fixed in:** v5.15-rc7
**Buggy since:** v5.14-rc1 (introduced by commit `f1a0a376ca0c` "sched/core: Initialize the idle task with preemption disabled")

## Bug Description

The Shadow Call Stack (SCS) is an ARM64 security feature that maintains a separate, hidden stack of return addresses alongside the normal call stack. On function entry, the return address is pushed to the shadow stack; on function return, the popped shadow address is compared with the actual return address to detect corruption (e.g., from ROP attacks). Each task maintains an SCS stack pointer (`thread_info.scs_sp`) that tracks the current position within its allocated SCS page.

When a CPU is hotplugged back online, the idle task for that CPU resumes execution starting from `__secondary_switched`, which loads the saved `scs_sp` value into the ARM64 x18 register via `scs_load()`. This becomes the new base for all subsequent shadow stack pushes on that CPU's idle thread. If the saved `scs_sp` was never reset between hotplug cycles, it retains the position it had at the end of the previous hotplug-off cycle. Each offline-online cycle therefore starts with an `scs_sp` that is higher than the previous cycle's starting point.

Prior to commit `f1a0a376ca0c`, the function `init_idle()` was called on every hotplug cycle via `idle_thread_get()`, and `init_idle()` invoked `scs_task_reset()` which resets `scs_sp` back to the base of the SCS page. When commit `f1a0a376ca0c` removed the `init_idle()` call from `idle_thread_get()` (since `smp_init()` already initializes idle tasks for all possible CPUs at boot), it inadvertently eliminated the only code path that reset the idle task's SCS stack pointer between hotplug cycles.

The result is a progressive SCS overflow: each CPU hotplug offline-online cycle advances the SCS stack pointer further into (and eventually past) the allocated SCS page, leading to a stack overflow once enough cycles have occurred.

## Root Cause

The root cause is the removal of `scs_task_reset()` from the CPU hotplug path without adding an equivalent reset elsewhere. The chain of events is:

1. **Before the regression:** `idle_thread_get()` called `init_idle()`, which called `scs_task_reset(idle)`. This reset `idle->thread_info.scs_sp` to `idle->thread_info.scs_base` (the base of the allocated SCS page). This happened on every CPU hotplug-up via the call chain `_cpu_up()` → `idle_thread_get()` → `init_idle()`.

2. **The breaking change (f1a0a376ca0c):** Valentin Schneider's commit changed `init_idle()` from a regular function to `__init` (meaning it is discarded after boot) and removed the `init_idle()` call from `idle_thread_get()`. The motivation was that `smp_init()` already initializes idle tasks for all possible CPUs, so re-initialization on each hotplug should be unnecessary. However, this overlooked the side effect that `scs_task_reset()` inside `init_idle()` was the only mechanism to reset the SCS pointer.

3. **The overflow mechanism:** When a CPU goes offline, `idle_task_exit()` is called on the departing CPU. The idle task's current `scs_sp` reflects all the call frames accumulated during the offline process. When the CPU is brought back online, `__secondary_switched` → `init_cpu_task` → `scs_load()` loads this stale `scs_sp` into the x18 register. Every subsequent function call during the startup and idle loop pushes more return addresses above this stale pointer. After enough hotplug cycles, the shadow stack pointer exceeds the allocated SCS page boundary, corrupting adjacent memory.

The specific field affected is `current->thread_info.scs_sp`, which is an `unsigned long *` pointing into the task's SCS page. The function `scs_task_reset()` (defined in `kernel/scs.c`) sets it back to `task->thread_info.scs_base`. Without this reset, each hotplug cycle monotonically increases the stored `scs_sp`.

## Consequence

The consequence of this bug is a Shadow Call Stack overflow on the idle task of any CPU that undergoes repeated hotplug cycles. The overflow has several impacts:

**Memory corruption:** Once `scs_sp` exceeds the SCS page boundary, return addresses are written into whatever memory follows the SCS page. This could corrupt adjacent kernel data structures, slab allocations, or page metadata. On ARM64 with `CONFIG_SHADOW_CALL_STACK=y`, the SCS page is typically 4 KiB (one page, accommodating ~512 return addresses). Given that each hotplug cycle adds several dozen return addresses (from the startup and shutdown call chains), overflow can occur after approximately 10-20 hotplug cycles on a given CPU.

**Potential kernel crash:** The corruption may trigger a variety of kernel panics or oopses depending on what adjacent memory is overwritten. The crash is not deterministic and depends on memory layout. On production Android devices (which commonly use ARM64 with SCS enabled and may hotplug CPUs for power management), this bug could cause intermittent crashes that are difficult to diagnose because the corruption site is far removed from the crash site.

**Security implications:** SCS exists as a security mitigation against return-oriented programming (ROP) attacks. An overflow of the shadow stack itself undermines this security boundary and could potentially be exploited by an attacker who can trigger repeated CPU hotplug events.

## Fix Summary

The fix adds a single line — `scs_task_reset(current)` — to the `idle_task_exit()` function in `kernel/sched/core.c`. This function is called when a CPU goes offline, on the idle task of the departing CPU (protected by `BUG_ON(current != this_rq()->idle)`).

By placing `scs_task_reset(current)` in `idle_task_exit()`, the idle task's SCS stack pointer is reset to its base every time the CPU goes offline. When the CPU is subsequently brought back online and `__secondary_switched` → `scs_load()` loads the `scs_sp`, it will find it pointing to the base of the SCS page, starting fresh. This prevents the progressive accumulation of stale SCS entries across hotplug cycles.

The fix is placed in `idle_task_exit()` (the offline path) rather than in the online path because `idle_task_exit()` runs on the idle task itself (`current`), making it a natural place to reset per-task state. As explained by the patch author, the reset only affects the saved `thread_info.scs_sp` value — the x18 register (which holds the live SCS pointer during execution) is not modified at this point, so the remaining function returns in the offline call chain are unaffected. The x18 register will be updated from the reset `scs_sp` later when `__secondary_switched` calls `scs_load()` during the next online sequence.

## Triggering Conditions

The bug requires all of the following conditions:

- **ARM64 architecture with Shadow Call Stack enabled:** `CONFIG_SHADOW_CALL_STACK=y`. This is an ARM64-specific feature; x86 does not have SCS. It is commonly enabled on Android devices.
- **CPU hotplug support:** `CONFIG_HOTPLUG_CPU=y`. The system must be able to take CPUs offline and bring them back online.
- **Repeated hotplug cycles on the same CPU:** The bug is cumulative — each hotplug cycle advances the SCS pointer. A single cycle does not overflow; approximately 10-20 cycles (depending on the call chain depth during startup/shutdown) are needed to exhaust the default 4 KiB SCS page.
- **Kernel version between v5.14-rc1 and v5.15-rc6 (inclusive):** The bug was introduced by `f1a0a376ca0c` in the v5.14 merge window and fixed by this commit in v5.15-rc7.

The bug does not require any special workload or timing. Any system that periodically hotplugs CPUs (e.g., Android power management, thermal throttling, or manual `echo 0 > /sys/devices/system/cpu/cpuN/online` cycles) will trigger it deterministically after enough iterations. There is no race condition involved — the overflow is a purely sequential, deterministic accumulation.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following fundamental reasons:

### 1. Why this bug cannot be reproduced with kSTEP

**Requires ARM64 architecture with Shadow Call Stack hardware support.** The Shadow Call Stack is an ARM64-specific feature that uses the x18 register as a dedicated SCS stack pointer. It is controlled by `CONFIG_SHADOW_CALL_STACK` which is only available on ARM64. kSTEP runs in QEMU with an x86_64 guest kernel. The x86_64 architecture does not implement Shadow Call Stack — the `scs_task_reset()` function compiles to a no-op on x86, and the `thread_info.scs_sp` / `thread_info.scs_base` fields do not exist. Therefore, even if the hotplug code path could be exercised, the bug's core mechanism (SCS pointer accumulation) simply does not exist on x86.

**Requires CPU hotplug lifecycle.** The bug manifests through the CPU offline → online cycle, specifically the `idle_task_exit()` function (called during CPU offline) and the `__secondary_switched` → `scs_load()` sequence (called during CPU online). kSTEP does not provide any API to simulate CPU hotplug events. There is no `kstep_cpu_offline()` or `kstep_cpu_online()` function, and the QEMU environment does not support dynamically removing and re-adding CPUs during a test run.

**Requires idle task execution context.** The bug specifically affects the per-CPU idle task (`rq->idle`). The SCS overflow occurs because the idle task is reused across hotplug cycles. kSTEP creates user-controlled tasks via `kstep_task_create()` and kthreads via `kstep_kthread_create()`, but it does not provide access to or control over the per-CPU idle tasks' execution context during hotplug transitions.

### 2. What would need to be added to kSTEP

To support this class of bugs, kSTEP would need:

- **ARM64 guest support:** kSTEP would need to build and run an ARM64 kernel in QEMU with `CONFIG_SHADOW_CALL_STACK=y`. This is a fundamental architecture change, not a minor extension — it requires cross-compilation toolchains, ARM64 QEMU machine configuration, and potentially different driver ABIs.
- **CPU hotplug simulation:** A `kstep_cpu_hotplug(cpu, online)` API that triggers the full CPU hotplug state machine, including `idle_task_exit()` on offline and `__secondary_switched` → `cpu_startup_entry()` on online. This would need to interface with the kernel's `_cpu_down()` / `_cpu_up()` functions.
- **SCS state inspection:** APIs to read `thread_info.scs_sp` and `thread_info.scs_base` for the idle task to detect the progressive overflow.

### 3. Version assessment

The bug exists in kernels v5.14-rc1 through v5.15-rc6, which falls within kSTEP's supported range (≥ v5.15). However, the architecture (ARM64) and CPU hotplug requirements are the blocking factors, not the kernel version.

### 4. Alternative reproduction methods

- **ARM64 hardware or QEMU ARM64:** Run an ARM64 kernel (v5.14 or v5.15-rc1 through rc6) with `CONFIG_SHADOW_CALL_STACK=y` and `CONFIG_HOTPLUG_CPU=y`. Execute repeated CPU hotplug cycles:
  ```bash
  for i in $(seq 1 50); do
    echo 0 > /sys/devices/system/cpu/cpu1/online
    sleep 0.1
    echo 1 > /sys/devices/system/cpu/cpu1/online
    sleep 0.1
  done
  ```
  Monitor for kernel crashes or memory corruption. Enable `CONFIG_KASAN` for earlier detection of the out-of-bounds SCS write.
- **Instrumented kernel:** Add a `printk` in `idle_task_exit()` to log `current->thread_info.scs_sp - current->thread_info.scs_base` and observe the value increasing with each hotplug cycle.
