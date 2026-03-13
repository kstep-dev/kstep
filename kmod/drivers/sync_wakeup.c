// https://github.com/torvalds/linux/commit/aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042

#include "driver.h"

static struct task_struct *waker_task;
static struct task_struct *wakee_task;
static struct task_struct *other_task;

static void setup(void) {
  other_task = kstep_task_create();

  // Waker: waker pinned to CPU 1; will call __wake_up_sync when triggered.
  waker_task = kstep_kthread_create("waker");
  kstep_kthread_bind(waker_task, cpumask_of(1));
  kstep_kthread_start(waker_task);
  kstep_kthread_yield(waker_task);

  // Wakee: sleeper, first restricted to CPU 2 so the scheduler sets
  // wake_cpu=2 on the initial placement.  This makes prev_cpu differ from
  // this_cpu inside __wake_up_sync, which is needed to trigger the bug.
  wakee_task = kstep_kthread_create("wakee");
  kstep_kthread_bind(wakee_task, cpumask_of(2));
  kstep_kthread_start(wakee_task);
  kstep_kthread_block(wakee_task);

  // Expand allowed set to CPUs 1,2 after the initial placement on CPU 2.
  struct cpumask mask;
  cpumask_copy(&mask, cpu_active_mask);
  cpumask_clear_cpu(0, &mask);
  kstep_kthread_bind(wakee_task, &mask);
}

static void *is_ineligible(void) {
  if (other_task->on_cpu && !kstep_eligible(&other_task->se))
    return other_task;
  return NULL;
}

static void run(void) {
  kstep_task_kernel_pin(other_task, 1, 1);
  kstep_task_kernel_wakeup(other_task);

  kstep_tick_repeat(20);

  // Tick until there is an ineligible task on CPU 1.
  kstep_tick_until(is_ineligible);

  // Trigger waker (on CPU 1) to call __wake_up_sync targeting wakee.
  kstep_kthread_syncwake(waker_task, wakee_task);

  // Pause the ineligible task
  kstep_task_kernel_pause(other_task);

  // tick to show the impact
  kstep_tick_repeat(10);
}

KSTEP_DRIVER_DEFINE{
    .name = "sync_wakeup",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 1000,
};
