// https://github.com/torvalds/linux/commit/8feb053d5319
//
// Bug: try_to_block_task() takes task_state by value. When signal_pending_state()
// returns true, it sets p->__state = TASK_RUNNING but doesn't update the caller's
// prev_state local variable. trace_sched_switch() then sees a stale sleeping state.
//
// Fix: Change try_to_block_task() to take a pointer to task_state so it can
// propagate the TASK_RUNNING update back to the caller.
//
// Detection: A counter added in __schedule() after try_to_block_task() detects
// when prev_state is stale (doesn't match actual __state). On the buggy kernel
// the counter increments; on the fixed kernel prev_state is correctly updated.

#include "internal.h"
#include <linux/kthread.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 14, 0)

static struct task_struct *victim;
static struct task_struct *spinner;

static volatile int phase; // 0=spin, 1=do_schedule, 2=done

static int victim_fn(void *data) {
  while (phase == 0)
    cpu_relax();

  // Set TASK_INTERRUPTIBLE and call schedule(). With TIF_SIGPENDING set,
  // __schedule -> try_to_block_task finds signal_pending_state() true, sets
  // __state = TASK_RUNNING, but (on buggy kernel) doesn't update prev_state.
  for (int i = 0; i < 20; i++) {
    set_current_state(TASK_INTERRUPTIBLE);
    schedule();
  }

  phase = 2;
  while (!kthread_should_stop())
    cpu_relax();
  return 0;
}

static int spinner_fn(void *data) {
  while (!kthread_should_stop())
    cpu_relax();
  return 0;
}

static void setup(void) {
  phase = 0;

  victim = kthread_create(victim_fn, NULL, "sw_victim");
  if (IS_ERR(victim))
    panic("kthread_create victim failed");
  kthread_bind(victim, 1);
  wake_up_process(victim);

  spinner = kthread_create(spinner_fn, NULL, "sw_spinner");
  if (IS_ERR(spinner))
    panic("kthread_create spinner failed");
  kthread_bind(spinner, 1);
  wake_up_process(spinner);
}

static void run(void) {
  kstep_tick_repeat(5);

  atomic_t *stale_count = kstep_ksym_lookup("kstep_stale_prev_state_count");
  if (!stale_count) {
    kstep_fail("cannot find kstep_stale_prev_state_count symbol");
    return;
  }
  atomic_set(stale_count, 0);

  TRACE_INFO("victim pid=%d state=0x%x",
             victim->pid, READ_ONCE(victim->__state));

  set_tsk_thread_flag(victim, TIF_SIGPENDING);
  phase = 1;

  kstep_tick_repeat(50);

  int count = atomic_read(stale_count);
  TRACE_INFO("phase=%d victim_state=0x%x stale_count=%d",
             phase, READ_ONCE(victim->__state), count);

  if (count > 0) {
    kstep_fail("stale prev_state detected %d times in __schedule()", count);
  } else if (phase == 2) {
    kstep_pass("no stale prev_state (victim completed %d schedule calls)", 20);
  } else {
    kstep_fail("victim did not complete schedule loop (phase=%d)", phase);
  }

  clear_tsk_thread_flag(victim, TIF_SIGPENDING);
  kstep_tick_repeat(5);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "sched_switch_prev_state",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
