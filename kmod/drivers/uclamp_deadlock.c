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

static int max_preempt_count_ske = -1;
static bool probe_armed = false;

static int ske_handler(struct kprobe *p, struct pt_regs *regs) {
  if (probe_armed) {
    int pc = preempt_count();
    if (pc > max_preempt_count_ske)
      max_preempt_count_ske = pc;
    TRACE_INFO("static_key_enable called with preempt_count=%d", pc);
  }
  return 0;
}

static struct kprobe ske_kp = {
    .symbol_name = "static_key_enable",
    .pre_handler = ske_handler,
};

static void setup(void) {}

static void run(void) {
  typedef int (*setattr_fn_t)(struct task_struct *, const struct sched_attr *);
  setattr_fn_t setattr_fn =
      (setattr_fn_t)kstep_ksym_lookup("sched_setattr_nocheck");
  if (!setattr_fn) {
    kstep_fail("could not resolve sched_setattr_nocheck");
    return;
  }

  int kp_ret = register_kprobe(&ske_kp);
  if (kp_ret < 0) {
    kstep_fail("register_kprobe on static_key_enable failed: %d", kp_ret);
    return;
  }

  struct task_struct *target = current;
  struct sched_attr attr = {
      .size = sizeof(attr),
      .sched_policy = target->policy,
      .sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN,
      .sched_util_min = 512,
      .sched_nice = task_nice(target),
  };

  max_preempt_count_ske = -1;
  probe_armed = true;

  TRACE_INFO("Calling sched_setattr_nocheck on current (pid=%d) "
             "with SCHED_FLAG_UTIL_CLAMP_MIN", target->pid);
  int ret = setattr_fn(target, &attr);
  probe_armed = false;

  unregister_kprobe(&ske_kp);

  if (ret) {
    kstep_fail("sched_setattr_nocheck failed: %d", ret);
    return;
  }

  unsigned int uclamp_min = target->uclamp_req[UCLAMP_MIN].value;
  TRACE_INFO("uclamp_req[MIN].value=%u (expected 512)", uclamp_min);
  TRACE_INFO("max preempt_count in static_key_enable: %d",
             max_preempt_count_ske);

  if (max_preempt_count_ske < 0) {
    kstep_fail("kprobe on static_key_enable never fired");
    return;
  }

  // On the buggy kernel: static_key_enable is called from
  // __setscheduler_uclamp under task_rq_lock (pi_lock + rq->lock),
  // so preempt bits (0-7) >= 2. On the fixed kernel: static_key_enable
  // is called from uclamp_validate before any locks, so preempt bits == 0.
  // The kprobe handler adds hardirq overhead to bits 16+, so mask those out.
  int preempt_bits = max_preempt_count_ske & 0xff;
  if (preempt_bits > 0) {
    kstep_fail("static_key_enable called with preempt_count=0x%x "
               "(preempt_bits=%d, under scheduler locks)",
               max_preempt_count_ske, preempt_bits);
  } else {
    kstep_pass("static_key_enable called with preempt_count=0x%x "
               "(preempt_bits=%d, no scheduler locks held)",
               max_preempt_count_ske, preempt_bits);
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
