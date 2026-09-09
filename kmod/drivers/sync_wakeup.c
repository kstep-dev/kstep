// https://github.com/torvalds/linux/commit/aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042
//
// A sync wakeup (WF_SYNC) tells the scheduler that the waker is about to sleep, so the
// wakee may take the waker's CPU. With EEVDF's delayed dequeue, a task paused while
// ineligible stays on the runqueue and still counts in nr_running, so wake_affine_idle()
// believes the waker's CPU will stay busy and places the wakee on its previous CPU; the
// waker then sleeps and its CPU goes idle while the wakee runs elsewhere.
//
// All three actors are user tasks. The sync wakeup is a real one: the waker writes into
// post (pipe_write -> wake_up_interruptible_sync_poll) from its own CPU.

#include "driver.h"
#include "internal.h" // cpu_rq, for the check after the wakeup

static struct task_struct *other, *waker, *wakee;

static void setup(void) {
  other = kstep_task_create();
  waker = kstep_task_create();
  wakee = kstep_task_create();

  // Waker on CPU 1, where it will issue the sync wakeup.
  kstep_task_pin(waker, 1, 1);

  // Wakee waits, first restricted to CPU 2 so its wake_cpu is 2
  // (prev_cpu != this_cpu inside the wakeup), then allowed on CPUs 1-2.
  kstep_task_pin(wakee, 2, 2);
  kstep_task_wait(wakee);
  kstep_task_pin(wakee, 1, 2);
}

static void *is_ineligible(void) {
  if (other->on_cpu && !kstep_eligible(&other->se))
    return other;
  return NULL;
}

static void run(void) {
  kstep_task_pin(other, 1, 1);
  kstep_task_wakeup(other);
  kstep_task_wakeup(waker);

  kstep_tick_repeat(20);

  // Tick until `other` is current on CPU 1 and ineligible, then pause it: EEVDF keeps it
  // queued (sched_delayed), so CPU 1's nr_running stays 2.
  kstep_tick_until(is_ineligible);
  kstep_task_pause(other);

  // Waker (CPU 1) posts: a sync wakeup of the wakee. Then it sleeps, as a sync waker is
  // expected to (with pause, not wait: it would consume its own token).
  kstep_task_post(waker);
  kstep_task_pause(waker);

  kstep_tick_repeat(1);
  if (cpu_rq(1)->curr == cpu_rq(1)->idle && task_cpu(wakee) != 1)
    TRACE_INFO("warn: sync wakeup placed the wakee on cpu %d while cpu 1 went idle",
               task_cpu(wakee));
  kstep_tick_repeat(9);
}

KSTEP_DRIVER_DEFINE{
    .name = "sync_wakeup",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 1000,
};
