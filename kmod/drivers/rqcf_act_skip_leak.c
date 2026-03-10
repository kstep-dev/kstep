// https://github.com/torvalds/linux/commit/5ebde09d91707a4a9bec1e3d213e3c12ffde348f
//
// Bug: RQCF_ACT_SKIP flag leaks from __schedule() into newidle_balance
// because it's only cleared very late (in context_switch or else branch).
// The fix clears it immediately after update_rq_clock() in __schedule().
//
// Reproduction: set REQ_SKIP on a CPU's rq, then have that CPU's sole task
// block (call schedule()). __schedule() promotes REQ_SKIP to ACT_SKIP.
// On the buggy kernel, ACT_SKIP persists into newidle_balance (observable
// in the on_sched_balance_begin callback). On the fixed kernel,
// rq->clock_update_flags = RQCF_UPDATED clears it immediately.

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 6, 0)

#define NUM_TASKS_CPU2 5

static struct task_struct *kthread1;
static struct task_struct *tasks_cpu2[NUM_TASKS_CPU2];
static atomic_t act_skip_detected = ATOMIC_INIT(0);
static atomic_t balance_checked = ATOMIC_INIT(0);

static void on_balance_begin(int cpu, struct sched_domain *sd) {
  if (cpu != 1)
    return;
  struct rq *rq = cpu_rq(cpu);
  unsigned int flags = rq->clock_update_flags;
  atomic_set(&balance_checked, 1);
  if (flags & RQCF_ACT_SKIP) {
    atomic_set(&act_skip_detected, 1);
    TRACE_INFO("RQCF_ACT_SKIP leak on CPU %d: flags=0x%x", cpu, flags);
  }
}

static void setup(void) {
  kthread1 = kstep_kthread_create("rqcf_kt1");
  kstep_kthread_bind(kthread1, cpumask_of(1));
  kstep_kthread_start(kthread1);

  for (int i = 0; i < NUM_TASKS_CPU2; i++)
    tasks_cpu2[i] = kstep_task_create();
}

static void run(void) {
  for (int i = 0; i < NUM_TASKS_CPU2; i++) {
    kstep_task_pin(tasks_cpu2[i], 2, 2);
    kstep_task_wakeup(tasks_cpu2[i]);
  }

  kstep_tick_repeat(10);

  struct rq *rq1 = cpu_rq(1);

  // Ensure rd->overload is set so newidle_balance proceeds past the early check
  WRITE_ONCE(rq1->rd->overload, 1);

  // Set REQ_SKIP on CPU1's rq. In normal operation this is set by
  // ttwu_do_wakeup when a wakeup targets a CPU whose current task
  // already has NEED_RESCHED.
  rq1->clock_update_flags |= RQCF_REQ_SKIP;

  // Block the kthread: wait_event -> schedule() -> __schedule().
  // __schedule() promotes REQ_SKIP -> ACT_SKIP (via <<= 1).
  // On the buggy kernel, ACT_SKIP leaks into newidle_balance.
  // On the fixed kernel, rq->clock_update_flags = RQCF_UPDATED clears it.
  kstep_kthread_block(kthread1);
  kstep_sleep();

  kstep_tick_repeat(10);

  if (atomic_read(&act_skip_detected))
    kstep_fail("RQCF_ACT_SKIP leak: flag visible during newidle_balance");
  else if (atomic_read(&balance_checked))
    kstep_pass("No RQCF_ACT_SKIP leak");
  else
    kstep_fail("newidle_balance did not fire on CPU1");
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
static void on_balance_begin(int cpu, struct sched_domain *sd) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "rqcf_act_skip_leak",
    .setup = setup,
    .run = run,
    .on_sched_balance_begin = on_balance_begin,
    .step_interval_us = 10000,
};
