# Core: Sleeping in Atomic Context Due to Static Key for sched_clock_irqtime

**Commit:** `f3fa0e40df175acd60b71036b9a1fd62310aec03`
**Affected files:** `kernel/sched/cputime.c`, `kernel/sched/sched.h`
**Fixed in:** v6.14-rc7
**Buggy since:** v6.14-rc1 (introduced by commit `8722903cbb8f` "sched: Define sched_clock_irqtime as static key")

## Bug Description

The `sched_clock_irqtime` variable controls whether the kernel performs IRQ time accounting via `sched_clock`. When enabled, the kernel tracks how much CPU time is spent servicing hardware and software interrupts, subtracting this from task execution time to provide more accurate per-task CPU time accounting. This variable is toggled by `enable_sched_clock_irqtime()` and `disable_sched_clock_irqtime()`, and is queried on every IRQ entry/exit via `irqtime_enabled()`.

Commit `8722903cbb8f` changed `sched_clock_irqtime` from a plain `int` to a `DEFINE_STATIC_KEY_FALSE` static key. The motivation was performance: since `irqtime_enabled()` is called on every IRQ entry and exit (a very hot path), using a static key allows the compiler to generate a NOP instruction that is patched at runtime, avoiding a memory load and branch on every IRQ. The `enable_sched_clock_irqtime()` function was changed to call `static_branch_enable(&sched_clock_irqtime)` and `disable_sched_clock_irqtime()` to call `static_branch_disable(&sched_clock_irqtime)`.

However, this optimization introduced a critical correctness bug: the static key enable/disable operations internally call `static_key_enable()`/`static_key_disable()`, which in turn call `cpus_read_lock()`. This function acquires a percpu read-write semaphore (`cpuhp_pin_lock`) and contains `might_sleep()`, meaning it is a potentially sleeping operation. Multiple code paths in the kernel call `disable_sched_clock_irqtime()` and `enable_sched_clock_irqtime()` from atomic contexts (preemption disabled, spinlock held, or IRQs disabled), where sleeping is illegal.

## Root Cause

The root cause is the mismatch between the calling context requirements of the static key API and the contexts from which `enable_sched_clock_irqtime()` / `disable_sched_clock_irqtime()` are invoked.

In the buggy kernel, `disable_sched_clock_irqtime()` is implemented as:

```c
void disable_sched_clock_irqtime(void)
{
    static_branch_disable(&sched_clock_irqtime);
}
```

`static_branch_disable()` expands to `static_key_disable()` in `kernel/jump_label.c`:

```c
void static_key_disable(struct static_key *key)
{
    cpus_read_lock();           // <-- might_sleep()!
    static_key_disable_cpuslocked(key);
    cpus_read_unlock();
}
```

The `cpus_read_lock()` call acquires `cpu_hotplug_lock`, a percpu rwsem. Acquiring this lock involves `percpu_down_read()`, which calls `might_sleep()` to validate that the caller is in a sleepable context. If `CONFIG_DEBUG_ATOMIC_SLEEP` is enabled, this emits a "BUG: sleeping function called from invalid context" warning. Without that config option, on PREEMPT kernels, the lock could actually block if contended, causing a "scheduling while atomic" BUG.

There are at least three code paths that call these functions from atomic context:

1. **KVM vCPU load path**: `vcpu_load()` calls `get_cpu()` which disables preemption, then calls `kvm_arch_vcpu_load()` → `mark_tsc_unstable()` → `disable_sched_clock_irqtime()`. This happens when KVM detects that the TSC went backwards (e.g., after host suspend/resume), meaning the TSC is unreliable across CPUs.

2. **Clocksource watchdog path**: `clocksource_watchdog()` holds `watchdog_lock` (a spinlock), then calls `__clocksource_unstable()` → `tsc_cs_mark_unstable()` → `disable_sched_clock_irqtime()`.

3. **sched_clock_register() path**: This function calls `local_irq_save(flags)` to disable IRQs, then conditionally calls `enable_sched_clock_irqtime()` if the sched_clock rate is fast enough. IRQs remain disabled during this call.

All three contexts are non-sleepable: preemption-disabled, spinlock-held, and IRQ-disabled respectively. The plain `int` version (`sched_clock_irqtime = 0` or `= 1`) is a simple store that works safely in any context, but the static key version requires sleeping lock acquisition.

## Consequence

The most immediate consequence is a kernel warning: "BUG: sleeping function called from invalid context" when `CONFIG_DEBUG_ATOMIC_SLEEP` is enabled. This warning is emitted by the `might_sleep()` check inside `cpus_read_lock()`. While a warning alone does not crash the system, it indicates a serious correctness violation.

On `CONFIG_PREEMPT` kernels, the consequence can be more severe. If the `cpu_hotplug_lock` is actually contended (e.g., during a concurrent CPU hotplug operation), `percpu_down_read()` will attempt to schedule to wait for the lock. Scheduling while in atomic context triggers a hard BUG: "BUG: scheduling while atomic", which typically results in a kernel panic or at minimum a non-recoverable kernel state on the affected CPU. The KVM path is particularly concerning because it occurs during normal vCPU operation whenever the host TSC appears to go backwards, which can happen after host suspend/resume — a scenario that is not exotic on laptop or cloud hosts.

Even if the lock is not contended, the static key patching operation itself (`static_key_disable_cpuslocked()`) modifies kernel text (patching NOP to branch or vice versa) and issues IPIs to synchronize all CPUs. Performing this from atomic context could cause deadlocks if the IPI handling interacts poorly with the context that disabled preemption or IRQs.

## Fix Summary

The fix in commit `f3fa0e40df175acd60b71036b9a1fd62310aec03` reverts the optimization from `8722903cbb8f` by changing `sched_clock_irqtime` back from a static key to a plain integer:

In `kernel/sched/cputime.c`, the declaration changes from:
```c
DEFINE_STATIC_KEY_FALSE(sched_clock_irqtime);
```
to:
```c
int sched_clock_irqtime;
```

The enable/disable functions change from calling `static_branch_enable()`/`static_branch_disable()` to simple integer assignments:
```c
void enable_sched_clock_irqtime(void)  { sched_clock_irqtime = 1; }
void disable_sched_clock_irqtime(void) { sched_clock_irqtime = 0; }
```

In `kernel/sched/sched.h`, the declaration changes from `DECLARE_STATIC_KEY_FALSE(sched_clock_irqtime)` to `extern int sched_clock_irqtime`, and `irqtime_enabled()` changes from `static_branch_likely(&sched_clock_irqtime)` to simply returning `sched_clock_irqtime`. This makes the enable/disable operations trivially safe in any context — an integer store never sleeps, takes locks, or issues IPIs. The tradeoff is a minor performance regression on the IRQ accounting hot path (a memory load + branch instead of a NOP), but correctness takes priority over this micro-optimization.

## Triggering Conditions

The bug can be triggered under any of the following conditions:

- **KVM with TSC instability**: A host running KVM where a vCPU migrates between physical CPUs and the TSC values differ between the CPUs (e.g., after host suspend/resume). When `kvm_arch_vcpu_load()` detects the TSC went backwards, it calls `mark_tsc_unstable()` with preemption disabled (via `get_cpu()` in `vcpu_load()`).

- **Clocksource watchdog detecting TSC instability**: The periodic `clocksource_watchdog()` timer detects that the TSC clocksource has deviated beyond acceptable limits. Under `watchdog_lock` (spinlock), it marks the TSC unstable, calling `disable_sched_clock_irqtime()`.

- **sched_clock registration**: During early boot or when a new sched_clock source is registered, `sched_clock_register()` is called with IRQs disabled. If the clock rate is fast enough (≥ 1MHz), it calls `enable_sched_clock_irqtime()`.

- **Kernel configuration**: The kernel must be built with `CONFIG_IRQ_TIME_ACCOUNTING=y` (which is common on x86 distro kernels). For the warning to be visible, `CONFIG_DEBUG_ATOMIC_SLEEP=y` is needed; for an actual crash, a PREEMPT kernel with a contended `cpu_hotplug_lock` is required.

- **Architecture**: x86 is the primary platform where TSC instability triggers the KVM and clocksource paths. The `sched_clock_register()` path applies to ARM and other architectures too.

For synthetic reproduction, any context that disables preemption, holds a spinlock, or disables IRQs and then calls `disable_sched_clock_irqtime()` or `enable_sched_clock_irqtime()` will trigger the bug. The call does not need to come from the real KVM or clocksource paths — the bug is in the function implementation itself.

## Reproduce Strategy (kSTEP)

The reproduction strategy leverages the fact that the bug is in the implementation of `disable_sched_clock_irqtime()` itself, not in any specific caller. We can directly invoke the function from atomic context within a kSTEP driver to demonstrate the sleeping-in-atomic violation.

### Prerequisites

1. The kernel must be built with `CONFIG_IRQ_TIME_ACCOUNTING=y` so that `disable_sched_clock_irqtime()` and `enable_sched_clock_irqtime()` exist (not compiled out to empty stubs).
2. The kernel should be built with `CONFIG_DEBUG_ATOMIC_SLEEP=y` to make the `might_sleep()` warning visible in dmesg/printk output. Without this, the bug may still exist but not produce observable output unless the lock is actually contended.

### Driver Design

1. **Import symbols**: Use `KSYM_IMPORT(disable_sched_clock_irqtime)` and `KSYM_IMPORT(enable_sched_clock_irqtime)` to obtain function pointers to the enable/disable functions. These are non-static symbols defined in `kernel/sched/cputime.c`.

2. **Setup**: No special topology is required. A single CPU is sufficient. No cgroups or special task configurations are needed.

3. **Trigger sequence**:
   - First, call `enable_sched_clock_irqtime()` from a normal (preemptible) context to ensure IRQ time accounting is enabled. This sets up the static key (on buggy kernel) or the int (on fixed kernel) to the enabled state.
   - Then, enter a non-preemptible context using `preempt_disable()` (or alternatively `spin_lock()` on a local spinlock, or `local_irq_save()`).
   - Call `disable_sched_clock_irqtime()` from within this atomic context.
   - Exit the atomic context with `preempt_enable()` (or the corresponding unlock/restore).

4. **Detection**:
   - On the **buggy kernel**: `disable_sched_clock_irqtime()` calls `static_branch_disable()` → `static_key_disable()` → `cpus_read_lock()` → `might_sleep()`. With `CONFIG_DEBUG_ATOMIC_SLEEP`, this prints a "BUG: sleeping function called from invalid context" message to the kernel log. The driver should scan for this message (e.g., by checking dmesg output or using `kstep_pass`/`kstep_fail` after the operation). If the kernel actually tries to sleep (on PREEMPT kernels with contended lock), it triggers "scheduling while atomic" which is even more visible.
   - On the **fixed kernel**: `disable_sched_clock_irqtime()` simply sets `sched_clock_irqtime = 0`, which is a plain integer store. No sleeping, no warning, no issue. The operation completes instantly.

5. **Pass/fail criteria**:
   - **FAIL (bug present)**: The kernel log contains "sleeping function called from invalid context" or "scheduling while atomic" after the `disable_sched_clock_irqtime()` call from atomic context.
   - **PASS (bug fixed)**: No warnings or errors in the kernel log. The integer store completes silently.

6. **Alternative detection method**: If `CONFIG_DEBUG_ATOMIC_SLEEP` is not available, we can measure the execution time of `disable_sched_clock_irqtime()`. On the buggy kernel, the static key patching involves `cpus_read_lock()`, potential IPI synchronization, and text patching — this takes microseconds to milliseconds. On the fixed kernel, an integer store takes nanoseconds. A timing-based check (e.g., using `ktime_get()` before and after) could distinguish the two implementations.

7. **Restore state**: After the test, call `enable_sched_clock_irqtime()` from a normal (preemptible) context to restore IRQ time accounting to its original enabled state, so as not to affect the rest of the system.

### Expected Output

- **Buggy kernel (before fix)**: The kernel should emit a warning like:
  ```
  BUG: sleeping function called from invalid context at kernel/cpu.c:XXX
  in_atomic(): 1, irqs_disabled(): 0, non_block: 0, pid: XXX, name: XXX
  preempt_count: 1, expected: 0
  ...
  Call Trace:
   cpus_read_lock
   static_key_disable
   disable_sched_clock_irqtime
   [kstep driver]
  ```
  The driver should detect this and call `kstep_fail("sleeping in atomic context detected")`.

- **Fixed kernel (after fix)**: No warning. The driver should verify no warning was emitted and call `kstep_pass("no sleeping in atomic context")`.

### kSTEP Modifications Needed

No modifications to kSTEP are required for this reproduction. The driver only needs:
- `KSYM_IMPORT` to access the `disable_sched_clock_irqtime` and `enable_sched_clock_irqtime` symbols.
- Standard kernel APIs (`preempt_disable()`, `preempt_enable()`) which are available in any kernel module.
- `printk` output or dmesg scanning to detect the warning.

The one potential issue is ensuring `CONFIG_IRQ_TIME_ACCOUNTING=y` and `CONFIG_DEBUG_ATOMIC_SLEEP=y` in the kSTEP kernel build configuration. If these are not set by default, they should be added to the kSTEP kernel config for this test.
