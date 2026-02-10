// https://github.com/torvalds/linux/commit/d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1

#include <linux/interrupt.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/version.h>

#include <asm/ptrace.h>

#include "driver.h"
#include "internal.h" // rq

/*
 * Bug summary (d4ac164bde7a):
 * Commit 85e511df3cec moved the slice-expiry reschedule out of update_deadline()
 * into update_curr() and (incorrectly) used rq->nr_running as the "single CFS
 * task" fast-path check:
 *
 *   if (rq->nr_running == 1) return;
 *
 * rq->nr_running counts non-CFS runnable tasks too (e.g., RT). If the current
 * fair task runs in a non-preemptible section (CONFIG_PREEMPT_NONE) while an RT
 * task is runnable, rq->nr_running > 1 while cfs_rq->nr_running == 1. This
 * causes resched_curr() to be called on slice expiry, even though there's no
 * other fair task to switch to.
 *
 * Fix changes the check back to cfs_rq->nr_running.
 *
 * Reproducer strategy:
 * - Run a CFS kernel thread on CPU1 that never calls schedule() (holds CPU1 in
 *   non-preemptible context).
 * - Make a FIFO user task runnable on CPU1. It will be enqueued but never run.
 * - Use a kprobe on resched_curr() to count calls from tick interrupt context
 *   while:
 *     rq->curr == cfs_holder, rq->cfs.nr_running == 1, rq->rt.rt_nr_running == 1.
 *
 * Expected:
 * - Buggy kernel (d4ac164~1): FAIL (hits > 0).
 * - Fixed kernel (d4ac164): PASS (hits == 0).
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)

static struct task_struct *cfs_holder;
static struct task_struct *rt_proc;

static atomic_t holder_ready = ATOMIC_INIT(0);
static atomic_t holder_go = ATOMIC_INIT(0);

static atomic_t resched_hits = ATOMIC_INIT(0);

static int holder_main(void *data) {
  if (smp_processor_id() != 1)
    pr_warn("over_schedule: holder unexpectedly started on CPU%d\n",
            smp_processor_id());

  atomic_set(&holder_ready, 1);

  /*
   * Wait until the driver tells us to start hogging CPU1. This runs before
   * kSTEP disables ticks / resets state, so keep it quiet and cooperative.
   */
  while (!atomic_read(&holder_go)) {
    set_current_state(TASK_INTERRUPTIBLE);
    schedule();
  }
  __set_current_state(TASK_RUNNING);

  /* Busy-loop forever; on CONFIG_PREEMPT_NONE this prevents switching to RT. */
  while (!kthread_should_stop())
    cpu_relax();

  return 0;
}

static int resched_curr_pre_handler(struct kprobe *kp, struct pt_regs *regs) {
  struct rq *rq = (struct rq *)regs_get_kernel_argument(regs, 0);

  if (smp_processor_id() != 1)
    return 0;

  if (!rq || rq->cpu != 1)
    return 0;

  /*
   * resched_curr() is used all over the scheduler; restrict to the buggy
   * scenario and to tick interrupt context.
   */
  if (!in_irq())
    return 0;

  if (rq->curr != cfs_holder)
    return 0;

  if (rq->cfs.nr_running != 1)
    return 0;

  if (rq->rt.rt_nr_running != 1)
    return 0;

  if (rq->nr_running != 2)
    return 0;

  atomic_inc(&resched_hits);
  return 0;
}

static struct kprobe resched_curr_kp = {
    .symbol_name = "resched_curr",
    .pre_handler = resched_curr_pre_handler,
};

static void dump_state(const char *tag) {
  struct rq *rq = cpu_rq(1);
  struct task_struct *curr = rq->curr;
  pr_info(
      "over_schedule: {\"tag\":\"%s\", \"hits\":%d, \"rq_nr_running\":%u, "
      "\"cfs_nr_running\":%u, \"rt_nr_running\":%u, \"rt_queued\":%d, "
      "\"curr_pid\":%d, \"curr_comm\":\"%s\", \"curr_policy\":%d, "
      "\"holder_pid\":%d, \"rt_pid\":%d}\n",
      tag, atomic_read(&resched_hits), rq->nr_running, rq->cfs.nr_running,
      rq->rt.rt_nr_running, rq->rt.rt_queued, curr ? curr->pid : -1,
      curr ? curr->comm : "?", curr ? curr->policy : -1,
      cfs_holder ? cfs_holder->pid : -1, rt_proc ? rt_proc->pid : -1);
}

static bool in_pathological_state(void) {
  struct rq *rq = cpu_rq(1);

  if (rq->curr != cfs_holder)
    return false;
  if (rq->cfs.nr_running != 1)
    return false;
  if (rq->rt.rt_nr_running != 1)
    return false;
  if (rq->nr_running != 2)
    return false;

  return true;
}

static void setup(void) {
  int ret;

  rt_proc = kstep_task_create();

  cfs_holder = kthread_create(holder_main, NULL, "eevdf_holder");
  if (IS_ERR(cfs_holder))
    panic("kthread_create(holder) failed: %ld", PTR_ERR(cfs_holder));

  kthread_bind(cfs_holder, 1);
  wake_up_process(cfs_holder);

  for (int i = 0; i < 200; i++) {
    if (atomic_read(&holder_ready))
      break;
    kstep_sleep();
  }

  if (!atomic_read(&holder_ready))
    panic("holder thread did not start");

  /* Set the user task to FIFO without requiring it to run in userspace. */
  KSYM_IMPORT(sched_setscheduler_nocheck);
  struct sched_param sp = {.sched_priority = 80};
  ret = KSYM_sched_setscheduler_nocheck(rt_proc, SCHED_FIFO, &sp);
  if (ret)
    panic("sched_setscheduler_nocheck(FIFO) failed: %d", ret);
}

static void run(void) {
  int ret = register_kprobe(&resched_curr_kp);
  if (ret < 0) {
    kstep_fail("register_kprobe(resched_curr) failed: %d", ret);
    return;
  }

  /* Start hogging CPU1, then wake the FIFO task so it stays queued. */
  atomic_set(&holder_go, 1);
  wake_up_process(cfs_holder);
  kstep_sleep();

  kstep_task_wakeup(rt_proc);
  kstep_tick_repeat(5);

  for (int i = 0; i < 200; i++) {
    if (in_pathological_state())
      break;
    kstep_tick();
  }

  if (!in_pathological_state()) {
    dump_state("bad_state");
    kstep_fail("did not reach expected state (cfs=1, rt=1, rq=2)");
    return; /* Leave the kprobe registered; the kernel is about to reboot. */
  }

  dump_state("ready");

  atomic_set(&resched_hits, 0);
  kstep_tick_repeat(50);

  dump_state("after_ticks");

  if (atomic_read(&resched_hits) > 0)
    kstep_fail("resched_curr() called with single CFS task (bug present)");
  else
    kstep_pass("no resched_curr() with single CFS task (bug fixed)");
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "over_schedule",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
