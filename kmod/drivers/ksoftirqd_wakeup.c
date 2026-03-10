// Reproduce: e932c4ab38f0 sched/core: Prevent wakeup of ksoftirqd during idle load balance
//
// Bug: nohz_csd_func() uses raise_softirq_irqoff(SCHED_SOFTIRQ) to raise
// a scheduling softirq during idle load balancing. When invoked from
// non-interrupt context, raise_softirq_irqoff() sees !in_interrupt() and
// unnecessarily wakes ksoftirqd, causing a wasteful context switch.
//
// Fix: Use __raise_softirq_irqoff() which just marks softirq pending.
//
// Strategy: Call nohz_csd_func() directly from the driver (CPU 0, process
// context) with an idle CPU's rq. Since CPU 2 has no tasks, idle_cpu(2)
// returns true. On buggy kernel, raise_softirq_irqoff -> wakeup_softirqd.
// On fixed kernel, __raise_softirq_irqoff -> no wakeup.

#include "driver.h"
#include "internal.h"

#include <linux/kprobes.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 13, 0)

typedef void (*nohz_csd_func_t)(void *);
static nohz_csd_func_t nohz_csd_fn;
typedef int (*idle_cpu_fn_t)(int cpu);
static idle_cpu_fn_t idle_cpu_fn;

static int in_nohz_csd;
static atomic_t nohz_wakeup_count = ATOMIC_INIT(0);

static int nohz_csd_entry_handler(struct kretprobe_instance *ri,
                                  struct pt_regs *regs) {
  in_nohz_csd = 1;
  return 0;
}

static int nohz_csd_exit_handler(struct kretprobe_instance *ri,
                                 struct pt_regs *regs) {
  in_nohz_csd = 0;
  return 0;
}

static struct kretprobe nohz_csd_krp = {
    .entry_handler = nohz_csd_entry_handler,
    .handler = nohz_csd_exit_handler,
    .kp.symbol_name = "nohz_csd_func",
    .maxactive = 8,
};

static int wakeup_pre_handler(struct kprobe *p, struct pt_regs *regs) {
  if (in_nohz_csd) {
    atomic_inc(&nohz_wakeup_count);
    TRACE_INFO("wakeup_softirqd from nohz_csd_func on CPU %d",
               smp_processor_id());
  }
  return 0;
}

static struct kprobe wakeup_kp = {
    .symbol_name = "wakeup_softirqd",
    .pre_handler = wakeup_pre_handler,
};

static void setup(void) {}

static void run(void) {
  nohz_csd_fn = (nohz_csd_func_t)kstep_ksym_lookup("nohz_csd_func");
  if (!nohz_csd_fn) {
    kstep_fail("nohz_csd_func not found - CONFIG_NO_HZ_COMMON not enabled?");
    return;
  }
  idle_cpu_fn = (idle_cpu_fn_t)kstep_ksym_lookup("idle_cpu");
  if (!idle_cpu_fn) {
    kstep_fail("idle_cpu not found");
    return;
  }

  int ret = register_kretprobe(&nohz_csd_krp);
  if (ret < 0) {
    kstep_fail("Failed to register nohz_csd_func kretprobe: %d", ret);
    return;
  }

  ret = register_kprobe(&wakeup_kp);
  if (ret < 0) {
    unregister_kretprobe(&nohz_csd_krp);
    kstep_fail("Failed to register wakeup_softirqd kprobe: %d", ret);
    return;
  }

  atomic_set(&nohz_wakeup_count, 0);

  struct rq *rq2 = cpu_rq(2);
  TRACE_INFO("idle_cpu(2)=%d in_interrupt=%d", idle_cpu_fn(2),
             !!in_interrupt());

  for (int i = 0; i < 10; i++) {
    atomic_set(&rq2->nohz_flags, NOHZ_BALANCE_KICK);

    local_irq_disable();
    nohz_csd_fn(rq2);
    set_softirq_pending(local_softirq_pending() & ~(1 << SCHED_SOFTIRQ));
    local_irq_enable();

    rq2->idle_balance = 0;
  }

  int count = atomic_read(&nohz_wakeup_count);

  unregister_kprobe(&wakeup_kp);
  unregister_kretprobe(&nohz_csd_krp);

  TRACE_INFO("nohz_csd_func wakeup_softirqd count: %d (of 10 calls)", count);

  if (count > 0) {
    kstep_fail("Unnecessary ksoftirqd wakeups from nohz_csd_func: %d", count);
  } else {
    kstep_pass("No unnecessary ksoftirqd wakeups from nohz_csd_func");
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
