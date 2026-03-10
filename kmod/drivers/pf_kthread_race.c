// https://github.com/torvalds/linux/commit/3a7956e25e1d
//
// Bug: TOCTOU race in kthread_is_per_cpu() / to_kthread().
// The pattern `(p->flags & PF_KTHREAD) && kthread_is_per_cpu(p)` is broken
// because between the external PF_KTHREAD check and the internal WARN_ON in
// to_kthread(), another CPU can clear PF_KTHREAD via begin_new_exec().
//
// Reproduce: Create a kthread, clear its PF_KTHREAD flag (simulating
// begin_new_exec), then call kthread_is_per_cpu(). On the buggy kernel,
// to_kthread() fires WARN_ON. On the fixed kernel, __to_kthread() handles
// it gracefully.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 12, 0)

static struct task_struct *kt;

static void setup(void) {
  kt = kstep_kthread_create("victim");
  kstep_kthread_bind(kt, cpumask_of(1));
  kstep_kthread_start(kt);
}

static void run(void) {
  typedef bool (*kthread_is_per_cpu_fn_t)(struct task_struct *);

  kstep_tick_repeat(5);

  kthread_is_per_cpu_fn_t is_per_cpu =
      (kthread_is_per_cpu_fn_t)kstep_ksym_lookup("kthread_is_per_cpu");
  if (!is_per_cpu) {
    kstep_fail("could not resolve kthread_is_per_cpu");
    return;
  }

  TRACE_INFO("kt pid=%d cpu=%d flags=0x%x PF_KTHREAD=%d",
             kt->pid, task_cpu(kt), kt->flags,
             !!(kt->flags & PF_KTHREAD));

  // Simulate begin_new_exec(): clear PF_KTHREAD from the task.
  // In the real race, the kthread itself does this on its own CPU while
  // another CPU is between the external flag check and to_kthread().
  kt->flags &= ~PF_KTHREAD;

  TRACE_INFO("Cleared PF_KTHREAD, flags=0x%x", kt->flags);

  // Call kthread_is_per_cpu() on the task without PF_KTHREAD.
  // Buggy kernel: to_kthread() -> WARN_ON(!(p->flags & PF_KTHREAD))
  // Fixed kernel: __to_kthread() -> returns NULL, no WARN
  bool result = is_per_cpu(kt);
  TRACE_INFO("kthread_is_per_cpu returned: %d", result);

  // Restore PF_KTHREAD so cleanup works
  kt->flags |= PF_KTHREAD;

  if (test_taint(TAINT_WARN))
    kstep_fail("WARN fired in to_kthread(): "
               "PF_KTHREAD vs to_kthread() race exposed");
  else
    kstep_pass("no WARN: __to_kthread() handled missing PF_KTHREAD");
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "pf_kthread_race",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
