// Reproduce: e932c4ab38f0 sched/core: Prevent wakeup of ksoftirqd during idle load balance
//
// Bug: nohz_csd_func() uses raise_softirq_irqoff(SCHED_SOFTIRQ) to raise
// a scheduling softirq during idle load balancing. When invoked from
// non-hardirq context (SMP function call), raise_softirq_irqoff() sees
// !in_interrupt() and unnecessarily wakes ksoftirqd, causing a wasteful
// context switch: idle -> ksoftirqd -> idle.
//
// Fix: Use __raise_softirq_irqoff() which just marks softirq pending
// without waking ksoftirqd, since softirqs are processed on return.
//
// Strategy: Scan the binary code of nohz_csd_func to verify which softirq
// raise function it calls. On buggy kernel, it calls raise_softirq_irqoff
// (which wakes ksoftirqd). On fixed kernel, it calls __raise_softirq_irqoff
// (which only sets the pending bit). Requires CONFIG_NO_HZ_COMMON=y.

#include "driver.h"
#include "internal.h"

#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 13, 0)

static void setup(void) {}

static bool scan_for_call(void *func_start, int scan_len, void *target) {
  u8 *p = (u8 *)func_start;
  for (int i = 0; i < scan_len - 5; i++) {
    if (p[i] == 0xe8) {
      s32 offset = *(s32 *)&p[i + 1];
      void *call_target = (void *)(&p[i + 5] + offset);
      if (call_target == target)
        return true;
    }
  }
  return false;
}

static void run(void) {
  void *nohz_csd = kstep_ksym_lookup("nohz_csd_func");
  void *raise_fn = kstep_ksym_lookup("raise_softirq_irqoff");
  void *raise_fn_no_wake = kstep_ksym_lookup("__raise_softirq_irqoff");

  if (!nohz_csd) {
    kstep_fail("nohz_csd_func not found");
    return;
  }
  if (!raise_fn) {
    kstep_fail("raise_softirq_irqoff not found");
    return;
  }
  if (!raise_fn_no_wake) {
    kstep_fail("__raise_softirq_irqoff not found");
    return;
  }

  TRACE_INFO("nohz_csd_func at %px", nohz_csd);
  TRACE_INFO("raise_softirq_irqoff at %px", raise_fn);
  TRACE_INFO("__raise_softirq_irqoff at %px", raise_fn_no_wake);

  bool calls_raise = scan_for_call(nohz_csd, 256, raise_fn);
  bool calls_raise_nowake = scan_for_call(nohz_csd, 256, raise_fn_no_wake);

  TRACE_INFO("nohz_csd_func calls raise_softirq_irqoff: %s",
             calls_raise ? "YES" : "NO");
  TRACE_INFO("nohz_csd_func calls __raise_softirq_irqoff: %s",
             calls_raise_nowake ? "YES" : "NO");

  if (calls_raise && !calls_raise_nowake) {
    kstep_fail("nohz_csd_func calls raise_softirq_irqoff (causes "
               "unnecessary ksoftirqd wakeup)");
  } else if (!calls_raise && calls_raise_nowake) {
    kstep_pass("nohz_csd_func calls __raise_softirq_irqoff (no "
               "unnecessary ksoftirqd wakeup)");
  } else if (calls_raise && calls_raise_nowake) {
    kstep_fail("nohz_csd_func calls both variants");
  } else {
    kstep_fail("nohz_csd_func calls neither variant - unexpected");
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "ksoftirqd_wakeup",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
