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

static struct task_struct *task;

// Kprobe state: capture max preempt_count during the test window
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

static void setup(void) {
  task = kstep_task_create();
  kstep_task_pin(task, 1, 1);
}

static void run(void) {
  kstep_task_wakeup(task);
  kstep_tick_repeat(3);

  int kp_ret = register_kprobe(&crl_kp);
  if (kp_ret < 0) {
    kstep_fail("register_kprobe on cpus_read_lock failed: %d", kp_ret);
    return;
  }

  typedef int (*setattr_fn_t)(struct task_struct *, const struct sched_attr *);
  setattr_fn_t setattr_fn =
      (setattr_fn_t)kstep_ksym_lookup("sched_setattr_nocheck");
  if (!setattr_fn) {
    unregister_kprobe(&crl_kp);
    kstep_fail("could not resolve sched_setattr_nocheck");
    return;
  }

  // Pre-enable sched_uclamp_used from a safe (non-atomic) context.
  // This prevents the actual deadlock: when __setscheduler_uclamp calls
  // static_key_enable under rq lock, the key is already enabled so
  // static_key_enable_cpuslocked returns early (no jump_label_update,
  // no text_mutex). But cpus_read_lock is still called, so our kprobe
  // will still capture the preempt_count to detect the bug.
  typedef void (*ske_fn_t)(struct static_key *);
  ske_fn_t ske_fn = (ske_fn_t)kstep_ksym_lookup("static_key_enable");
  struct static_key *uclamp_key =
      (struct static_key *)kstep_ksym_lookup("sched_uclamp_used");
  if (!ske_fn || !uclamp_key) {
    unregister_kprobe(&crl_kp);
    kstep_fail("could not resolve static_key_enable or sched_uclamp_used");
    return;
  }
  ske_fn(uclamp_key);
  TRACE_INFO("Pre-enabled sched_uclamp_used to prevent actual deadlock");

  struct sched_attr attr = {
      .size = sizeof(attr),
      .sched_policy = task->policy,
      .sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN,
      .sched_util_min = 512,
      .sched_nice = task_nice(task),
  };

  // Arm the probe and call sched_setattr_nocheck.
  // On buggy kernel: cpus_read_lock is called from static_key_enable
  //   inside __setscheduler_uclamp (under task_rq_lock -> preempt elevated)
  // On fixed kernel: cpus_read_lock is called from static_key_enable
  //   inside uclamp_validate (before any locks -> preempt is low)
  max_preempt_count = -1;
  probe_armed = true;

  TRACE_INFO("Calling sched_setattr_nocheck with SCHED_FLAG_UTIL_CLAMP_MIN");
  int ret = setattr_fn(task, &attr);
  probe_armed = false;

  unregister_kprobe(&crl_kp);

  if (ret) {
    kstep_fail("sched_setattr_nocheck failed: %d", ret);
    return;
  }

  unsigned int uclamp_min = task->uclamp_req[UCLAMP_MIN].value;
  TRACE_INFO("uclamp_req[MIN].value=%u (expected 512)", uclamp_min);
  TRACE_INFO("max preempt_count in cpus_read_lock during setattr: %d",
             max_preempt_count);

  if (max_preempt_count < 0) {
    kstep_fail("kprobe on cpus_read_lock never fired during setattr");
    return;
  }

  // Kprobe handler adds +1 to preempt_count. Under task_rq_lock, pi_lock
  // and rq->lock each add +1, plus IRQ disable. So buggy path has
  // preempt_count >= 3 in the handler; fixed path has preempt_count <= 1.
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
