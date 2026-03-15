// Reproduce: select_idle_sibling kthread stacking uses raw nr_running
// without subtracting delayed tasks (same anti-pattern as aa3ee4f0b754).
//
// The per-cpu kthread stacking check this_rq()->nr_running <= 1 fails when
// a delayed-dequeued task inflates nr_running, preventing the wakee from
// being stacked on the kthread's CPU. This forces select_idle_cpu() fallback,
// breaking IO completion cache affinity.
//
// Key difference from sync_wakeup driver: wakee's prev_cpu == this_cpu,
// which exercises the kthread stacking path in select_idle_sibling rather
// than the wake_affine_idle path.

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

static struct task_struct *other_task;
static struct task_struct *waker_task;
static struct task_struct *wakee_task;

static void setup(void) {
  other_task = kstep_task_create();

  // Per-cpu kthread waker bound to CPU 1
  waker_task = kstep_kthread_create("waker");
  kstep_kthread_bind(waker_task, cpumask_of(1));
  kstep_kthread_start(waker_task);
  kstep_kthread_yield(waker_task);

  // Wakee: initially on CPU 1 so prev_cpu == 1 == smp_processor_id() of waker.
  // This triggers the kthread stacking path (prev == smp_processor_id())
  // in select_idle_sibling, NOT wake_affine_idle.
  wakee_task = kstep_kthread_create("wakee");
  kstep_kthread_bind(wakee_task, cpumask_of(1));
  kstep_kthread_start(wakee_task);
  kstep_kthread_block(wakee_task);

  // Expand wakee's cpumask so scheduler has alternative CPUs
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
  kstep_task_pin(other_task, 1, 1);
  kstep_task_wakeup(other_task);

  kstep_tick_repeat(20);

  // Tick until other_task is ineligible on CPU 1
  kstep_tick_until(is_ineligible);

  struct rq *rq1 = cpu_rq(1);
  TRACE_INFO("Before syncwake: nr_running=%u, h_nr_queued=%u, h_nr_runnable=%u",
             rq1->nr_running, rq1->cfs.h_nr_queued, rq1->cfs.h_nr_runnable);

  // Set up sync wakeup BEFORE pausing other_task.
  // The kthread will execute it after other_task sleeps (delayed dequeue).
  kstep_kthread_syncwake(waker_task, wakee_task);

  // Pause other_task -> task entity delayed dequeue -> nr_running inflated
  kstep_task_pause(other_task);

  // After pause: kthread picks up do_syncwakeup and calls __wake_up_sync.
  // At that moment, nr_running includes the delayed entity.
  // The kthread stacking check in select_idle_sibling:
  //   this_rq()->nr_running <= 1  (BUGGY: uses raw nr_running)
  // should be:
  //   (this_rq()->nr_running - cfs_h_nr_delayed(this_rq())) <= 1

  // Give time for the sync wakeup to execute
  kstep_tick_repeat(5);

  TRACE_INFO("After syncwake: nr_running=%u, h_nr_queued=%u, h_nr_runnable=%u",
             rq1->nr_running, rq1->cfs.h_nr_queued, rq1->cfs.h_nr_runnable);

  // Check where the wakee was placed
  int wakee_cpu = task_cpu(wakee_task);
  TRACE_INFO("wakee placed on CPU %d (expected CPU 1 with fix)", wakee_cpu);

  if (wakee_cpu == 1) {
    kstep_pass("wakee stacked on kthread CPU %d", wakee_cpu);
  } else {
    kstep_fail("wakee on CPU %d not 1 (kthread stacking bypassed)", wakee_cpu);
  }

  kstep_tick_repeat(5);
}

static void on_tick_begin(void) {
  struct rq *rq1 = cpu_rq(1);
  unsigned int delayed = rq1->cfs.h_nr_queued - rq1->cfs.h_nr_runnable;
  kstep_json_print_2kv("type", "nr_running", "val", "%u/%u",
                       rq1->nr_running, delayed);
}

KSTEP_DRIVER_DEFINE{
    .name = "fair_wake_affine_delayed_dequeue",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 1000,
};

#endif
