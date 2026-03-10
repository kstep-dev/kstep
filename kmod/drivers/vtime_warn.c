// https://github.com/torvalds/linux/commit/f1dfdab694eb
//
// Bug: vtime->state is read multiple times without READ_ONCE() inside
// seqcount-protected sections of kcpustat_cpu_fetch_vtime(). Concurrent
// context switches on the target CPU can change vtime->state between reads,
// causing the code to fall through to WARN_ON_ONCE(vtime->state != VTIME_GUEST)
// when the state was actually transitioning (e.g., VTIME_SYS -> VTIME_INACTIVE).
//
// Reproduce: Create a kthread on CPU 1 that rapidly toggles its own
// vtime->state, while the driver on CPU 0 calls kcpustat_cpu_fetch() for
// CPU 1 in a tight loop. The WARN_ON_ONCE fires when state changes mid-read.

#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/kernel_stat.h>
#include <linux/kthread.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 6, 0) && defined(CONFIG_VIRT_CPU_ACCOUNTING_GEN)

#define TARGET_CPU 1

static struct task_struct *toggler_task;
static volatile int stop_toggling;

// Kthread that rapidly cycles vtime->state to create the race window
static int vtime_toggler_fn(void *data) {
  struct vtime *vt = &current->vtime;

  while (!READ_ONCE(stop_toggling)) {
    // Cycle through active vtime states to maximize race window
    // The kcpustat reader expects state to stay consistent within
    // a seqcount section, but we're changing it rapidly
    WRITE_ONCE(vt->state, VTIME_SYS);
    cpu_relax();
    WRITE_ONCE(vt->state, VTIME_USER);
    cpu_relax();
    WRITE_ONCE(vt->state, VTIME_SYS);
    cpu_relax();
    WRITE_ONCE(vt->state, VTIME_INACTIVE);
    cpu_relax();
    WRITE_ONCE(vt->state, VTIME_SYS);
    cpu_relax();
  }

  return 0;
}

static void setup(void) {}

static void run(void) {
  struct kernel_cpustat dst;
  int iterations = 200000;
  struct rq *target_rq = cpu_rq(TARGET_CPU);

  stop_toggling = 0;

  // Create kthread on CPU 1 that toggles vtime->state
  toggler_task = kthread_create(vtime_toggler_fn, NULL, "vt_toggle");
  if (IS_ERR(toggler_task)) {
    kstep_fail("failed to create toggler kthread");
    return;
  }
  kthread_bind(toggler_task, TARGET_CPU);
  wake_up_process(toggler_task);

  // Give the kthread a moment to start running on CPU 1
  udelay(500);

  // Debug: check that toggler is actually running on CPU 1
  {
    struct task_struct *curr = READ_ONCE(target_rq->curr);
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
    TRACE_INFO("CPU %d curr: pid=%d comm=%s vtime.state=%d vtime.cpu=%d",
               TARGET_CPU, curr->pid, curr->comm,
               READ_ONCE(curr->vtime.state), READ_ONCE(curr->vtime.cpu));
#else
    TRACE_INFO("CPU %d curr: pid=%d comm=%s",
               TARGET_CPU, curr->pid, curr->comm);
#endif
    TRACE_INFO("toggler pid=%d on_cpu=%d", toggler_task->pid,
               toggler_task->on_cpu);
  }

  // Hammer kcpustat_cpu_fetch for CPU 1 from CPU 0
  for (int i = 0; i < iterations; i++) {
    kcpustat_cpu_fetch(&dst, TARGET_CPU);
    kcpustat_field(&kcpustat_cpu(TARGET_CPU), CPUTIME_SYSTEM, TARGET_CPU);
    kcpustat_field(&kcpustat_cpu(TARGET_CPU), CPUTIME_USER, TARGET_CPU);
    kcpustat_field(&kcpustat_cpu(TARGET_CPU), CPUTIME_GUEST, TARGET_CPU);
  }

  WRITE_ONCE(stop_toggling, 1);
  msleep(50);

  TRACE_INFO("kcpustat_cpu_fetch completed %d iterations", iterations);
  kstep_pass("completed %d kcpustat reads racing with vtime toggling",
             iterations);
}

KSTEP_DRIVER_DEFINE{
    .name = "vtime_warn",
    .setup = setup,
    .run = run,
    .step_interval_us = 100,
};

#else
KSTEP_DRIVER_DEFINE{
    .name = "vtime_warn",
};
#endif
