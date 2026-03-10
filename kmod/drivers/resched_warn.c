// https://github.com/torvalds/linux/commit/8061b9f5e111
//
// Bug: resched_latency_warn() uses WARN() to emit debug messages about
// scheduler latency. When panic_on_warn is enabled, this causes unintended
// kernel panics for what should be a debug diagnostic.
//
// Fix: Replace WARN() with pr_err() + dump_stack(), which still provides
// the debug information without triggering panic_on_warn.
//
// Reproduce: Enable the LATENCY_WARN sched feature, create a spinning
// kthread on CPU 1, set need_resched on it, and tick until the latency
// threshold is exceeded. On the buggy kernel, WARN() fires and taints
// the kernel with TAINT_WARN. On the fixed kernel, pr_err() is used
// which does not set the taint.

#include "internal.h"
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 13, 0)

static struct task_struct *spinner;

static void setup(void) {
  // Enable LATENCY_WARN sched feature
  KSYM_IMPORT(sysctl_sched_features);
  *KSYM_sysctl_sched_features |= (1UL << __SCHED_FEAT_LATENCY_WARN);

  // Lower threshold to 10ms for faster triggering
  KSYM_IMPORT(sysctl_resched_latency_warn_ms);
  *KSYM_sysctl_resched_latency_warn_ms = 10;

  // Create a kthread that spins in kernel space on CPU 1.
  // With PREEMPT_NONE, this kthread will not be preempted, so
  // need_resched will persist across scheduler ticks.
  spinner = kstep_kthread_create("spinner");
  kstep_kthread_bind(spinner, cpumask_of(1));
  kstep_kthread_start(spinner);
}

static void run(void) {
  // Let the spinner settle on CPU 1
  kstep_tick_repeat(3);

  struct task_struct *curr1 = cpu_rq(1)->curr;
  TRACE_INFO("CPU 1 curr: pid=%d comm=%s", curr1->pid, curr1->comm);

  // Set need_resched on CPU 1's current task.
  // With PREEMPT_NONE the kthread never calls schedule(), so
  // TIF_NEED_RESCHED stays set across subsequent ticks.
  set_tsk_need_resched(curr1);

  // Tick 30 times (30ms virtual time > 10ms threshold).
  // scheduler_tick() on CPU 1 will call cpu_resched_latency() which
  // detects need_resched has been set too long, and calls
  // resched_latency_warn().
  // Buggy: WARN()  -> TAINT_WARN set
  // Fixed: pr_err() + dump_stack() -> TAINT_WARN NOT set
  kstep_tick_repeat(30);

  if (test_taint(TAINT_WARN))
    kstep_fail("WARN() fired in resched_latency_warn: "
               "causes panic when panic_on_warn=1");
  else
    kstep_pass("pr_err() used in resched_latency_warn: "
               "safe with panic_on_warn=1");
}

KSTEP_DRIVER_DEFINE{
    .name = "resched_warn",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 1000,
    .tick_interval_ns = 1000000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "resched_warn",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
