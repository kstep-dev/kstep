/*
 * Reproduce: switched_to_dl() uses rq->donor instead of task_current()
 * for PELT sync under CONFIG_SCHED_PROXY_EXEC.
 *
 * Reference: fecfcbc288e9 ("sched/rt: Fix RT utilization tracking during
 * policy change")
 *
 * Strategy:
 *  1. Run a CFS task on CPU 1 for 20 ticks (avg_dl stays near 0).
 *  2. Advance the virtual clock 200 ticks WITHOUT running sched_tick,
 *     creating a gap where avg_dl.last_update_time falls behind.
 *  3. With CONFIG_SCHED_PROXY_EXEC=y, install a kprobe+kretprobe on
 *     switched_to_dl that temporarily sets rq->donor to a decoy task,
 *     simulating proxy execution (rq->curr != rq->donor).
 *  4. Change the task to SCHED_DEADLINE.  In the buggy code path
 *     (rq->donor != p), update_dl_rq_load_avg is skipped, leaving
 *     avg_dl.last_update_time 200 ticks stale.
 *  5. Tick once.  task_tick_dl -> update_dl_rq_load_avg sees the full
 *     200-tick gap as "DL running time", producing a huge util_avg spike.
 *  6. On the fixed kernel (task_current check), the PELT is synced during
 *     switched_to_dl, the gap is accounted as idle, and no spike occurs.
 */

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

#include <linux/kprobes.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>

#include "driver.h"
#include "internal.h"

static struct task_struct *target_task;
static struct task_struct *decoy_task;

#define CLOCK_GAP_TICKS 200

/* ---------- kprobe + kretprobe on switched_to_dl ---------- */

#ifdef CONFIG_SCHED_PROXY_EXEC

static struct task_struct *saved_donor;
static bool kprobe_armed;

/*
 * kprobe pre-handler: runs before switched_to_dl body.
 * Set rq->donor to decoy to simulate proxy execution.
 */
static int switched_to_dl_entry(struct kprobe *kp, struct pt_regs *regs) {
  struct rq *rq;
  struct task_struct *p;

  if (!kprobe_armed)
    return 0;

#ifdef CONFIG_X86_64
  rq = (struct rq *)regs->di;
  p = (struct task_struct *)regs->si;
#elif defined(CONFIG_ARM64)
  rq = (struct rq *)regs->regs[0];
  p = (struct task_struct *)regs->regs[1];
#else
  return 0;
#endif

  if (p != target_task)
    return 0;

  saved_donor = rq->donor;
  rq->donor = decoy_task;
  TRACE_INFO("kprobe-entry: rq->donor=%d (was %d) curr=%d p=%d",
             decoy_task->pid, saved_donor->pid, rq->curr->pid, p->pid);
  return 0;
}

static struct kprobe switched_to_dl_kp = {
    .symbol_name = "switched_to_dl",
    .pre_handler = switched_to_dl_entry,
};

/*
 * kretprobe handler: runs when switched_to_dl returns.
 * Restore rq->donor to avoid corrupting scheduler state.
 */
static int switched_to_dl_return(struct kretprobe_instance *ri,
                                 struct pt_regs *regs) {
  struct rq *rq;

  if (!saved_donor)
    return 0;

  rq = cpu_rq(task_cpu(target_task));
  if (rq->donor == decoy_task) {
    TRACE_INFO("kretprobe: restoring rq->donor=%d (from %d)", saved_donor->pid,
               decoy_task->pid);
    rq->donor = saved_donor;
  }
  kprobe_armed = false;
  return 0;
}

static struct kretprobe switched_to_dl_krp = {
    .handler = switched_to_dl_return,
    .kp.symbol_name = "switched_to_dl",
    .maxactive = 1,
};

static bool proxy_exec_available = true;
#else
static bool proxy_exec_available = false;
#endif /* CONFIG_SCHED_PROXY_EXEC */

static void setup(void) {
  target_task = kstep_task_create();
  kstep_task_pin(target_task, 1, 1);

  decoy_task = kstep_task_create();
  kstep_task_pin(decoy_task, 1, 1);

#ifdef CONFIG_SCHED_PROXY_EXEC
  int ret;
  ret = register_kprobe(&switched_to_dl_kp);
  if (ret < 0) {
    TRACE_INFO("kprobe registration failed: %d", ret);
    proxy_exec_available = false;
    return;
  }
  ret = register_kretprobe(&switched_to_dl_krp);
  if (ret < 0) {
    TRACE_INFO("kretprobe registration failed: %d", ret);
    unregister_kprobe(&switched_to_dl_kp);
    proxy_exec_available = false;
    return;
  }
  TRACE_INFO("kprobe+kretprobe on switched_to_dl registered");
#endif
}

static void set_task_deadline(struct task_struct *p) {
  struct sched_attr attr = {
      .size = sizeof(attr),
      .sched_policy = SCHED_DEADLINE,
      .sched_runtime = 5000000,
      .sched_deadline = 10000000,
      .sched_period = 10000000,
  };
  int ret = sched_setattr_nocheck(p, &attr);
  if (ret)
    TRACE_INFO("sched_setattr_nocheck failed: %d", ret);
}

static void run(void) {
  struct rq *rq = cpu_rq(1);

  kstep_task_wakeup(target_task);
  kstep_task_wakeup(decoy_task);

  /* Run 20 ticks so PELT stabilises; avg_dl stays near 0 (CFS task) */
  kstep_tick_repeat(20);

  /* Pause decoy so target_task is rq->curr on CPU 1 */
  kstep_task_pause(decoy_task);
  kstep_tick_repeat(2);

  u64 dl_last_update_before = rq->avg_dl.last_update_time;
  TRACE_INFO("Before gap: avg_dl.last_update=%llu util=%lu",
             dl_last_update_before, rq->avg_dl.util_avg);

  /*
   * Advance the virtual clock by CLOCK_GAP_TICKS ticks WITHOUT
   * running the scheduler tick.  avg_dl.last_update_time falls behind.
   */
  for (int i = 0; i < CLOCK_GAP_TICKS; i++) {
    kstep_sched_clock_tick();
    kstep_jiffies_tick();
  }

  TRACE_INFO("After gap: sched_clock=%llu avg_dl.last_update=%llu",
             kstep_sched_clock_get(), rq->avg_dl.last_update_time);

#ifdef CONFIG_SCHED_PROXY_EXEC
  if (proxy_exec_available)
    kprobe_armed = true;
#endif

  /* Switch target_task from CFS to SCHED_DEADLINE */
  set_task_deadline(target_task);

  u64 dl_last_update_after = rq->avg_dl.last_update_time;
  TRACE_INFO("After DL switch: avg_dl.last_update=%llu util=%lu",
             dl_last_update_after, rq->avg_dl.util_avg);

  /* One tick to trigger task_tick_dl -> update_dl_rq_load_avg */
  kstep_tick();

  unsigned long dl_util_after_tick = rq->avg_dl.util_avg;
  TRACE_INFO("After 1 tick: avg_dl.util_avg=%lu", dl_util_after_tick);

  /*
   * Detection:
   * - Buggy kernel: PELT update skipped in switched_to_dl.
   *   The 200-tick gap is all counted as DL-running on the next tick,
   *   producing util_avg > 500.
   * - Fixed kernel: PELT synced in switched_to_dl with running=0.
   *   The gap is accounted as idle.  After 1 tick of DL running,
   *   util_avg is small (< 200).
   */
  if (proxy_exec_available) {
    if (dl_util_after_tick > 500) {
      kstep_fail("PELT spike: avg_dl.util_avg=%lu (>500) "
                 "- stale last_update not synced in "
                 "switched_to_dl (donor vs curr bug)",
                 dl_util_after_tick);
    } else {
      kstep_pass("avg_dl.util_avg=%lu (<= 500) "
                 "- PELT properly synced",
                 dl_util_after_tick);
    }
  } else {
    if (dl_util_after_tick > 500) {
      kstep_fail("Unexpected spike without proxy exec: "
                 "avg_dl.util_avg=%lu",
                 dl_util_after_tick);
    } else {
      kstep_pass("No proxy exec: donor==curr always, "
                 "PELT correctly synced. util=%lu",
                 dl_util_after_tick);
    }
  }

#ifdef CONFIG_SCHED_PROXY_EXEC
  if (proxy_exec_available) {
    unregister_kretprobe(&switched_to_dl_krp);
    unregister_kprobe(&switched_to_dl_kp);
  }
#endif

  /* Only do extra ticks if state is not corrupted */
  if (dl_util_after_tick <= 500)
    kstep_tick_repeat(10);
}

KSTEP_DRIVER_DEFINE{
    .name = "pelt_rt_policy_change_spike",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#endif /* LINUX_VERSION_CODE >= 6.19.0 */
