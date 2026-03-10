// https://github.com/torvalds/linux/commit/e65855a52b47
//
// Bug: __setscheduler_uclamp() calls static_branch_enable(&sched_uclamp_used)
// while holding task_rq_lock (pi_lock + rq->lock). static_branch_enable()
// internally calls cpus_read_lock(), a sleeping function, from atomic context.
// This triggers "BUG: sleeping function called from invalid context".
//
// Fix: Move static_branch_enable() from __setscheduler_uclamp() to
// uclamp_validate(), which runs before any scheduler locks are acquired.
//
// Reproduce: Use a kprobe on static_key_enable to capture preempt_count
// when it is called during sched_setattr_nocheck(). On the buggy kernel,
// static_key_enable is called from __setscheduler_uclamp under rq lock
// so preempt_count is elevated. On the fixed kernel, it is called from
// uclamp_validate before locks are acquired.

#include "driver.h"
#include "internal.h"
#include <linux/kprobes.h>
#include <linux/version.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

static struct task_struct *task;

// Kprobe state: capture preempt_count when static_key_enable is called
static int captured_preempt_count = -1;
static bool probe_armed = false;

static int sbe_pre_handler(struct kprobe *p, struct pt_regs *regs) {
  if (probe_armed) {
    // preempt_count() here includes +1 from kprobe itself disabling preemption.
    // Under task_rq_lock (buggy path), raw_spin_lock adds more.
    captured_preempt_count = preempt_count();
    probe_armed = false;
  }
  return 0;
}

static struct kprobe sbe_kp = {
    .symbol_name = "static_key_enable",
    .pre_handler = sbe_pre_handler,
};

static void setup(void) {
  task = kstep_task_create();
  kstep_task_pin(task, 1, 1);
}

static void run(void) {
  kstep_task_wakeup(task);
  kstep_tick_repeat(3);

  int kp_ret = register_kprobe(&sbe_kp);
  if (kp_ret < 0) {
    kstep_fail("register_kprobe on static_key_enable failed: %d", kp_ret);
    return;
  }

  typedef int (*setattr_fn_t)(struct task_struct *, const struct sched_attr *);
  setattr_fn_t setattr_fn =
      (setattr_fn_t)kstep_ksym_lookup("sched_setattr_nocheck");
  if (!setattr_fn) {
    unregister_kprobe(&sbe_kp);
    kstep_fail("could not resolve sched_setattr_nocheck");
    return;
  }

  struct sched_attr attr = {
      .size = sizeof(attr),
      .sched_policy = task->policy,
      .sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN,
      .sched_util_min = 512,
      .sched_nice = task_nice(task),
  };

  // Arm the probe and call sched_setattr_nocheck.
  // On buggy kernel: static_key_enable is called from __setscheduler_uclamp
  //   (under task_rq_lock -> preempt_count elevated by spinlocks)
  // On fixed kernel: static_key_enable is called from uclamp_validate
  //   (before any scheduler locks -> preempt_count is low)
  captured_preempt_count = -1;
  probe_armed = true;

  TRACE_INFO("Calling sched_setattr_nocheck with SCHED_FLAG_UTIL_CLAMP_MIN");
  int ret = setattr_fn(task, &attr);

  unregister_kprobe(&sbe_kp);

  if (ret) {
    kstep_fail("sched_setattr_nocheck failed: %d", ret);
    return;
  }

  unsigned int uclamp_min = task->uclamp_req[UCLAMP_MIN].value;
  TRACE_INFO("uclamp_req[MIN].value=%u (expected 512)", uclamp_min);
  TRACE_INFO("captured preempt_count in static_key_enable: %d",
             captured_preempt_count);

  if (captured_preempt_count < 0) {
    // Probe never fired - static_key_enable might have been skipped
    // (e.g., already enabled via different path). Try scanning code instead.
    TRACE_INFO("kprobe did not fire, falling back to code scan");

    void *uclamp_fn = kstep_ksym_lookup("__setscheduler_uclamp");
    void *sbe_fn = kstep_ksym_lookup("static_key_enable");

    if (!uclamp_fn || !sbe_fn) {
      kstep_fail("could not resolve __setscheduler_uclamp or static_key_enable");
      return;
    }

    unsigned char *fn_start = (unsigned char *)uclamp_fn;
    bool found = false;
    for (int i = 0; i < 512 - 4; i++) {
      if (fn_start[i] == 0xE8) {
        int32_t offset;
        memcpy(&offset, &fn_start[i + 1], 4);
        void *target = (void *)(fn_start + i + 5 + offset);
        if (target == sbe_fn) {
          found = true;
          break;
        }
      }
    }

    if (found) {
      kstep_fail("static_key_enable called inside __setscheduler_uclamp "
                 "(under rq lock) - sleeping in atomic context");
    } else {
      kstep_pass("static_key_enable not in __setscheduler_uclamp");
    }
    return;
  }

  // Kprobe handler adds +1 to preempt_count. Under task_rq_lock, pi_lock
  // and rq->lock each add +1, plus IRQ disable. So buggy path has
  // preempt_count >= 3 in the handler; fixed path has preempt_count == 1.
  if (captured_preempt_count > 1) {
    kstep_fail("static_key_enable called with preempt_count=%d "
               "(under scheduler locks - sleeping in atomic context)",
               captured_preempt_count);
  } else {
    kstep_pass("static_key_enable called with preempt_count=%d "
               "(no scheduler locks held)",
               captured_preempt_count);
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
