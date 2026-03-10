// https://github.com/torvalds/linux/commit/e65855a52b47
//
// Bug: __setscheduler_uclamp() calls static_branch_enable(&sched_uclamp_used)
// while holding task_rq_lock (pi_lock + rq->lock). static_branch_enable()
// calls cpus_read_lock(), a sleeping function, from atomic context.
// This triggers "BUG: sleeping function called from invalid context".
//
// Fix: Move static_branch_enable() from __setscheduler_uclamp() to
// uclamp_validate(), which runs before any scheduler locks are acquired.
//
// Reproduce: Use a kprobe on cpus_read_lock to capture preempt_count when
// it is called during sched_setattr_nocheck(). On the buggy kernel,
// cpus_read_lock is called via static_key_enable from __setscheduler_uclamp
// (under rq lock) so preempt_count is elevated. On the fixed kernel, it
// is called from uclamp_validate before locks, so preempt_count is low.

#include "driver.h"
#include "internal.h"
#include <linux/kprobes.h>
#include <linux/version.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

static int max_preempt_count = -1;
static bool probe_armed = false;

static int cpus_read_lock_handler(struct kprobe *p, struct pt_regs *regs) {
  if (probe_armed) {
    int pc = preempt_count();
    if (pc > max_preempt_count)
      max_preempt_count = pc;
  }
  return 0;
}

static struct kprobe crl_kp = {
    .symbol_name = "cpus_read_lock",
    .pre_handler = cpus_read_lock_handler,
};

static void setup(void) {}

static void run(void) {
  // Use a kthread as the target for sched_setattr_nocheck.
  // This avoids user-space task issues with signal-based communication.
  struct task_struct *kt = kstep_kthread_create("uclamp_test");
  kstep_kthread_start(kt);

  typedef int (*setattr_fn_t)(struct task_struct *, const struct sched_attr *);
  setattr_fn_t setattr_fn =
      (setattr_fn_t)kstep_ksym_lookup("sched_setattr_nocheck");
  if (!setattr_fn) {
    kstep_fail("could not resolve sched_setattr_nocheck");
    return;
  }

  // Pre-enable sched_uclamp_used from a safe (non-atomic) context.
  // This prevents actual deadlock while still allowing the kprobe
  // to detect that cpus_read_lock is called under scheduler locks.
  typedef void (*ske_fn_t)(struct static_key *);
  ske_fn_t ske_fn = (ske_fn_t)kstep_ksym_lookup("static_key_enable");
  struct static_key *uclamp_key =
      (struct static_key *)kstep_ksym_lookup("sched_uclamp_used");
  if (!ske_fn || !uclamp_key) {
    kstep_fail("could not resolve static_key_enable or sched_uclamp_used");
    return;
  }
  ske_fn(uclamp_key);
  TRACE_INFO("Pre-enabled sched_uclamp_used to prevent actual deadlock");

  int kp_ret = register_kprobe(&crl_kp);
  if (kp_ret < 0) {
    kstep_fail("register_kprobe on cpus_read_lock failed: %d", kp_ret);
    return;
  }

  struct sched_attr attr = {
      .size = sizeof(attr),
      .sched_policy = kt->policy,
      .sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN,
      .sched_util_min = 512,
      .sched_nice = task_nice(kt),
  };

  max_preempt_count = -1;
  probe_armed = true;

  TRACE_INFO("Calling sched_setattr_nocheck with SCHED_FLAG_UTIL_CLAMP_MIN");
  int ret = setattr_fn(kt, &attr);
  probe_armed = false;

  unregister_kprobe(&crl_kp);

  if (ret) {
    kstep_fail("sched_setattr_nocheck failed: %d", ret);
    return;
  }

  unsigned int uclamp_min = kt->uclamp_req[UCLAMP_MIN].value;
  TRACE_INFO("uclamp_req[MIN].value=%u (expected 512)", uclamp_min);
  TRACE_INFO("max preempt_count in cpus_read_lock during setattr: %d",
             max_preempt_count);

  if (max_preempt_count < 0) {
    kstep_fail("kprobe on cpus_read_lock never fired during setattr");
    return;
  }

  // On the buggy kernel: cpus_read_lock is called from static_key_enable
  // inside __setscheduler_uclamp (under task_rq_lock -> preempt elevated).
  // pi_lock and rq->lock each disable preemption, so preempt_count >= 2.
  // On the fixed kernel: cpus_read_lock is called from uclamp_validate
  // before any locks, so preempt_count is 0 or 1.
  if (max_preempt_count > 1) {
    kstep_fail("cpus_read_lock called with preempt_count=%d "
               "(under scheduler locks - sleeping in atomic context)",
               max_preempt_count);
  } else {
    kstep_pass("cpus_read_lock called with preempt_count=%d "
               "(no scheduler locks held)",
               max_preempt_count);
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "uclamp_deadlock",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "uclamp_deadlock",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
