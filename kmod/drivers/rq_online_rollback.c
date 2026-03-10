// https://github.com/torvalds/linux/commit/fe7a11c78d2a
//
// Bug: In sched_cpu_deactivate(), if cpuset_cpu_inactive() fails and the
// error path is taken, the earlier sched_set_rq_offline() is not undone.
// This leaves rq->online = 0 while the CPU is still active, causing
// inconsistent scheduler state (RT/DL push/pull won't work for this CPU).
//
// Fix: Add sched_set_rq_online(rq, cpu) to the error rollback path.
//
// Reproduce: Make cpuset_cpu_inactive() fail by inflating dl_bw.total_bw
// so dl_bw_check_overflow() returns -EBUSY. Call sched_cpu_deactivate()
// with synchronize_rcu() temporarily patched to a no-op (kSTEP freezes
// timers, preventing RCU grace periods from completing).
// On the buggy kernel, rq->online stays 0 after the failed deactivation.
// On the fixed kernel, rq->online is restored to 1.

#include "internal.h"
#include <asm/special_insns.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 11, 0)

KSYM_IMPORT_TYPED(struct cpumask, __cpu_active_mask);

// Temporarily replace a function's first byte with RET (0xc3).
// Uses raw CR0 manipulation to bypass kernel's WP pinning.
static unsigned char patch_to_ret(void *func_addr) {
  unsigned char saved = *(unsigned char *)func_addr;
  unsigned long cr0;
  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  asm volatile("mov %0, %%cr0" :: "r"(cr0 & ~X86_CR0_WP) : "memory");
  *(unsigned char *)func_addr = 0xc3; // RET
  asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
  return saved;
}

static void restore_byte(void *func_addr, unsigned char saved) {
  unsigned long cr0;
  asm volatile("mov %%cr0, %0" : "=r"(cr0));
  asm volatile("mov %0, %%cr0" :: "r"(cr0 & ~X86_CR0_WP) : "memory");
  *(unsigned char *)func_addr = saved;
  asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}

static void setup(void) {}

static void run(void) {
  typedef int (*deactivate_fn_t)(unsigned int);
  deactivate_fn_t deactivate_fn =
      (deactivate_fn_t)kstep_ksym_lookup("sched_cpu_deactivate");
  if (!deactivate_fn) {
    kstep_fail("cannot resolve sched_cpu_deactivate");
    return;
  }

  void *sync_rcu_addr = kstep_ksym_lookup("synchronize_rcu");
  void *nohz_exit_addr = kstep_ksym_lookup("nohz_balance_exit_idle");
  if (!sync_rcu_addr || !nohz_exit_addr) {
    kstep_fail("cannot resolve synchronize_rcu or nohz_balance_exit_idle");
    return;
  }

  struct rq *rq1 = cpu_rq(1);

  TRACE_INFO("rq1->online=%d before test", rq1->online);
  if (!rq1->online) {
    kstep_fail("rq1 already offline before test");
    return;
  }

  // Inflate dl_bw.total_bw so dl_bw_check_overflow(1) returns -EBUSY
  struct dl_bw *dl_b = &rq1->rd->dl_bw;
  u64 saved_total_bw = dl_b->total_bw;

  TRACE_INFO("dl_bw: bw=%llu total_bw=%llu", dl_b->bw, dl_b->total_bw);

  raw_spin_lock(&dl_b->lock);
  dl_b->total_bw = dl_b->bw + 1;
  raw_spin_unlock(&dl_b->lock);

  TRACE_INFO("inflated dl_bw.total_bw to %llu", dl_b->total_bw);

  // Patch synchronize_rcu and nohz_balance_exit_idle to no-ops.
  // kSTEP freezes timers, so synchronize_rcu hangs indefinitely.
  // nohz_balance_exit_idle triggers WARN when called from wrong CPU.
  unsigned char saved_rcu = patch_to_ret(sync_rcu_addr);
  unsigned char saved_nohz = patch_to_ret(nohz_exit_addr);

  int ret = deactivate_fn(1);

  // Restore patched functions immediately
  restore_byte(sync_rcu_addr, saved_rcu);
  restore_byte(nohz_exit_addr, saved_nohz);

  TRACE_INFO("sched_cpu_deactivate(1) returned %d", ret);
  TRACE_INFO("rq1->online=%d after deactivate", rq1->online);
  TRACE_INFO("cpu_active(1)=%d",
             cpumask_test_cpu(1, KSYM___cpu_active_mask));

  // Restore dl_bw
  raw_spin_lock(&dl_b->lock);
  dl_b->total_bw = saved_total_bw;
  raw_spin_unlock(&dl_b->lock);

  if (ret == 0) {
    // Deactivation succeeded unexpectedly - need to reactivate
    typedef int (*activate_fn_t)(unsigned int);
    activate_fn_t activate_fn =
        (activate_fn_t)kstep_ksym_lookup("sched_cpu_activate");
    if (activate_fn)
      activate_fn(1);
    kstep_fail("expected sched_cpu_deactivate to fail but it succeeded");
    return;
  }

  // Buggy kernel: rq->online == 0 (sched_set_rq_online not called in rollback)
  // Fixed kernel: rq->online == 1 (sched_set_rq_online called in rollback)
  if (rq1->online == 0) {
    kstep_fail("rq->online=0 after failed deactivation: "
               "sched_set_rq_online missing from error rollback path");

    // Fix up: manually bring rq back online
    typedef void (*set_rq_online_fn_t)(struct rq *);
    set_rq_online_fn_t online_fn =
        (set_rq_online_fn_t)kstep_ksym_lookup("set_rq_online");
    if (online_fn) {
      unsigned long flags;
      raw_spin_lock_irqsave(&rq1->__lock, flags);
      if (rq1->rd)
        online_fn(rq1);
      raw_spin_unlock_irqrestore(&rq1->__lock, flags);
    }
    TRACE_INFO("manually restored rq1->online=%d", rq1->online);
  } else {
    kstep_pass("rq->online=1 after failed deactivation: "
               "sched_set_rq_online correctly called in error rollback");
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "rq_online_rollback",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
