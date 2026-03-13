# Core: Stale SCS and KASAN Stack State Across CPU Hotplug Cycles

**Commit:** `dce1ca0525bfdc8a69a9343bc714fbc19a2f04b3`
**Affected files:** kernel/sched/core.c, kernel/cpu.c
**Fixed in:** v5.16-rc3
**Buggy since:** v5.14-rc1 (commit f1a0a376ca0c4ef1 "sched/core: Initialize the idle task with preemption disabled")

## Bug Description

When a CPU is hot-unplugged, the per-CPU idle task calls several layers of C code before the CPU finally goes offline. Two pieces of state accumulate during this process: (1) KASAN-poisoned shadow entries are left on the stack for each active stack frame, and (2) the task's shadow call stack (SCS) stack pointer (`thread_info.scs_sp`) is left pointing at an arbitrary position within the shadow call stack rather than being reset to the base.

When the CPU is subsequently brought back online, this stale state causes problems. Stale KASAN shadow can alias new stack frames on the newly-onlined CPU, producing bogus "stack-out-of-bounds" KASAN warnings. The stale SCS SP means that a portion of the shadow call stack is effectively leaked — the task resumes using the SCS from wherever it left off, progressively consuming more of the shadow call stack page with each hotplug cycle until the entire SCS is exhausted.

The root cause is a refactoring in commit f1a0a376ca0c4ef1 ("sched/core: Initialize the idle task with preemption disabled") which restructured `init_idle()`. Prior to that commit, `init_idle()` was called on every CPU bringup and would reset both the SCS SP (via `scs_task_reset()`) and the KASAN stack shadow (via `kasan_unpoison_task_stack()`). The refactoring made `init_idle()` a one-time `__init` function, so these resets no longer happened on subsequent hotplug cycles. An interim fix in commit 63acd42c0d4942f7 ("sched/scs: Reset the shadow stack when idle_task_exit") attempted to fix the SCS issue by resetting in `idle_task_exit()` (called during CPU offline), but this was fragile because the reset happened in the context of the task being offlined, and it did not address the KASAN issue at all.

## Root Cause

The bug originates from the interaction between three code paths:

1. **`init_idle()` in `kernel/sched/core.c`**: This function initializes the idle task for a given CPU. Before commit f1a0a376ca0c, it contained calls to `scs_task_reset(idle)` and `kasan_unpoison_task_stack(idle)`, which reset the SCS stack pointer to the base of the shadow call stack and cleared any poisoned KASAN shadow on the task's stack. Because `init_idle()` was called each time a CPU was brought online (including hotplug cycles), these resets happened reliably. Commit f1a0a376ca0c refactored `init_idle()` to be an `__init` function called only once during boot, removing these resets from the hotplug path.

2. **`idle_task_exit()` in `kernel/sched/core.c`**: This function is called by the idle task of a CPU that is going offline. After commit 63acd42c0d4942f7, it contained a call to `scs_task_reset(current)` to reset the SCS SP during CPU offline. However, this reset happens while the idle task is still executing C code on its way to being offlined, meaning the register holding the active SCS SP (x18 on arm64) continues to advance past the reset saved value. More critically, `kasan_unpoison_task_stack()` was never added here, so the KASAN issue remained completely unfixed.

3. **`bringup_cpu()` in `kernel/cpu.c`**: This function is called by the CPU doing the bring-up (typically CPU 0) to online another CPU. It calls `idle_thread_get(cpu)` to get the idle task for the target CPU, then calls `__cpu_up(cpu, idle)` to actually bring the CPU online. Before the fix, there was no stack state cleanup between getting the idle task and booting the CPU. The idle task was passed to the architecture-specific `__cpu_up()` with whatever stale SCS SP and KASAN shadow it had from the previous offline cycle.

The specific failure mode for SCS is: each time a CPU is offlined and then onlined, the idle task's `thread_info.scs_sp` points further into the shadow call stack page. On arm64, `__secondary_switched` loads the saved `scs_sp` into register x18 and uses it as the new SCS base. Since it was never reset, each cycle starts from a higher point in the SCS, effectively leaking the lower portion. After enough cycles, the entire SCS page is consumed and the task overflows it.

For KASAN, the failure mode is: when the idle task was executing on the offlined CPU, KASAN poisoned the shadow memory corresponding to its stack frames. When the CPU comes back online and the idle task starts executing again with the same stack, new stack frames can overlap with old poisoned shadow entries, causing false-positive KASAN "stack-out-of-bounds" reports. The KASAN report from Qian Cai showed exactly this: a stack-out-of-bounds read in `vsnprintf` called from `_printk` during `cpuinfo_store_cpu` on the `secondary_start_kernel` path — the very first code the idle task runs after being brought online.

## Consequence

The most immediately visible consequence is bogus KASAN warnings on arm64 systems with `CONFIG_KASAN=y` and `CONFIG_KASAN_STACK=y`. The reporter (Qian Cai) observed a "BUG: KASAN: stack-out-of-bounds in vsnprintf" on CPU 0 (swapper/0) during the `secondary_start_kernel` → `cpuinfo_store_cpu` → `__cpuinfo_store_cpu` → `_printk` call chain. This produces false positive KASAN reports that pollute kernel logs and block regression testing, since developers cannot distinguish real bugs from aliased-shadow artifacts. The KASAN report showed a `Read of size 8 at addr ffff800016297db8` within the `_printk` stack frame where shadow memory showed `f3` (stack frame redzones) from a previous execution's poisoning.

For shadow call stacks (`CONFIG_SHADOW_CALL_STACK=y`, arm64-specific), the consequence is a progressive memory leak of the SCS page. Each hotplug cycle consumes additional SCS entries without reclaiming the old ones. After enough offline/online cycles, the entire 4KB shadow call stack page is exhausted, causing an SCS overflow. On arm64 with SCS, an overflow writes past the end of the SCS page, which can corrupt adjacent memory or trigger a fault depending on page layout. This is a potential security issue since SCS is a security feature designed to protect return addresses.

In production environments where CPUs are dynamically managed (e.g., power management, cloud VM hot-add/remove, CPU isolation), repeated hotplug cycles are common. The SCS leak is deterministic and cumulative — it will eventually overflow given enough cycles. The KASAN issue affects every hotplug cycle immediately, making kernel testing with KASAN + CPU hotplug unreliable on arm64.

## Fix Summary

The fix moves the `scs_task_reset()` and `kasan_unpoison_task_stack()` calls from `init_idle()` (where they ran once at boot) and `idle_task_exit()` (where `scs_task_reset` ran during offline) to `bringup_cpu()` in `kernel/cpu.c`. Specifically, the fix adds these two calls at the beginning of `bringup_cpu()`, immediately after obtaining the idle task reference via `idle_thread_get(cpu)` and before calling `__cpu_up()`:

```c
static int bringup_cpu(unsigned int cpu)
{
    struct task_struct *idle = idle_thread_get(cpu);
    int ret;

    /*
     * Reset stale stack state from the last time this CPU was online.
     */
    scs_task_reset(idle);
    kasan_unpoison_task_stack(idle);
    ...
}
```

Simultaneously, the fix removes the `scs_task_reset(idle)` and `kasan_unpoison_task_stack(idle)` calls from `init_idle()` (since `dup_task_struct()` already provides clean SCS SP and unpoisoned stack for newly created tasks), and removes `scs_task_reset(current)` from `idle_task_exit()` (since the reset now happens during CPU bringup instead).

This approach is more robust than the previous fixes for three reasons: (1) the reset happens on the bringing-up CPU (typically CPU 0), not on the idle task being offlined, so there is no risk of the task continuing to modify state after the reset; (2) it handles both SCS and KASAN in a single consistent location; (3) it runs at exactly the right time — after the last offline cycle's state is stale but before the CPU boots with the idle task.

## Triggering Conditions

The bug requires all of the following conditions:

- **CPU hotplug**: The system must support CPU hotplug, and at least one CPU must be offlined and then onlined again. The bug manifests on the first hotplug cycle and worsens with each subsequent cycle (for SCS). The reproduction script in the commit message repeatedly cycles through all CPUs: `echo 0 > /sys/devices/system/cpu/cpuN/online` followed by `echo 1 > /sys/devices/system/cpu/cpuN/online`.

- **KASAN and/or SCS enabled**: The KASAN symptom requires `CONFIG_KASAN=y` and `CONFIG_KASAN_STACK=y`. The SCS symptom requires `CONFIG_SHADOW_CALL_STACK=y`, which is only available on arm64. At least one of these must be enabled to observe the bug.

- **Kernel version**: The bug was introduced in v5.14-rc1 (commit f1a0a376ca0c) and fixed in v5.16-rc3. Any kernel in this range running on a system with the above config options is affected.

- **Multi-CPU system**: The system must have at least 2 CPUs since CPU 0 typically cannot be offlined. The architecture must support CPU hotplug (arm64, x86_64, etc.).

The bug is fully deterministic — every single hotplug cycle triggers it. There are no race conditions or timing dependencies. The KASAN symptom appears immediately on the first hotplug cycle. The SCS leak accumulates linearly with each cycle.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following fundamental reasons:

1. **CPU hotplug is not supported by kSTEP.** The core mechanism required to trigger this bug is CPU offline/online cycling via the kernel's CPU hotplug state machine (`cpu_down()` / `cpu_up()` or writing to `/sys/devices/system/cpu/cpuN/online`). kSTEP provides no API to simulate CPU hotplug events. There is no `kstep_cpu_hotplug()`, `kstep_cpu_offline()`, or equivalent function. The kSTEP topology APIs (`kstep_topo_init`, `kstep_topo_apply`) configure the initial topology but do not support dynamically adding or removing CPUs at runtime.

2. **The bug is in `kernel/cpu.c`, not the scheduler.** The actual bug and fix are in `bringup_cpu()` which is part of the CPU hotplug state machine in `kernel/cpu.c`, not in the scheduler core. While `init_idle()` and `idle_task_exit()` in `kernel/sched/core.c` are also modified, they are only incidentally involved — the real issue is the lack of stack state reset during CPU bringup. kSTEP operates within the scheduler subsystem and cannot intercept or invoke the CPU hotplug machinery.

3. **Shadow call stacks are an architecture-specific security feature.** SCS (`CONFIG_SHADOW_CALL_STACK`) is currently only available on arm64 and relies on a dedicated register (x18) to track the shadow stack pointer. kSTEP runs inside QEMU, which can emulate arm64, but the SCS mechanism is deeply tied to the CPU context switching and startup paths (`__secondary_switched` on arm64), not to scheduler logic. kSTEP cannot control or observe the SCS register or the `thread_info.scs_sp` field in a meaningful way through its current API.

4. **KASAN stack poisoning is a compiler/runtime feature.** The KASAN stack instrumentation automatically poisons/unpoisons shadow memory as stack frames are entered and exited. This is controlled by the compiler instrumentation and the KASAN runtime, not by the scheduler. kSTEP cannot control KASAN poisoning behavior or observe KASAN shadow memory state.

5. **Adding CPU hotplug to kSTEP would require fundamental changes.** To support this bug, kSTEP would need: (a) a `kstep_cpu_offline(cpu)` API that triggers the full `cpu_down()` path including `idle_task_exit()`, balance_push, migration of tasks, and all hotplug notifiers; (b) a `kstep_cpu_online(cpu)` API that triggers `bringup_cpu()` → `__cpu_up()` → the full CPU online path including secondary CPU startup; (c) proper integration with the QEMU virtual CPU management to actually park and unpark vCPUs. This is not a minor extension — it requires interfacing with the CPU hotplug state machine which has dozens of states and callbacks across multiple subsystems.

6. **Alternative reproduction methods:** The bug can be reproduced on any arm64 system (or QEMU arm64 VM) running a kernel between v5.14-rc1 and v5.16-rc2 with `CONFIG_KASAN=y CONFIG_KASAN_STACK=y` (for the KASAN symptom) or `CONFIG_SHADOW_CALL_STACK=y` (for the SCS symptom) by running the hotplug cycling script from the commit message: `while true; do for C in /sys/devices/system/cpu/cpu*/online; do echo 0 > $C; echo 1 > $C; done; done`. The KASAN warning will appear in `dmesg` within the first cycle. For SCS, the leak can be observed by checking the idle task's `scs_sp` value (via debugfs or printk instrumentation) across multiple cycles — it should monotonically increase until overflow.
