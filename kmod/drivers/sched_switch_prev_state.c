// https://github.com/torvalds/linux/commit/8feb053d5319
//
// Bug: try_to_block_task() takes task_state by value. When signal_pending_state()
// returns true, it sets p->__state = TASK_RUNNING but doesn't update the caller's
// prev_state local variable. trace_sched_switch() then sees a stale sleeping state.
//
// Fix: Change try_to_block_task() to take a pointer to task_state so it can
// propagate the TASK_RUNNING update back to the caller.
//
// Detection: A kernel-side counter (kstep_stale_prev_state_count) is incremented
// in __schedule after try_to_block_task() when prev_state is stale. On the buggy
// kernel the counter increments; on the fixed kernel prev_state is correctly updated.

#include "internal.h"
#include <linux/kthread.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 14, 0)

static struct task_struct *victim;
static volatile int phase;

static int victim_fn(void *data) {
  while (phase == 0)
    cpu_relax();

  for (int i = 0; i < 20; i++) {
    set_current_state(TASK_INTERRUPTIBLE);
    schedule();
  }

  phase = 2;
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
}

static void run(void) {
  kstep_tick_repeat(5);

  atomic_t *stale_count = kstep_ksym_lookup("kstep_stale_prev_state_count");
  if (!stale_count) {
    kstep_fail("cannot find kstep_stale_prev_state_count symbol");
    return;
  }
  atomic_set(stale_count, 0);

  TRACE_INFO("victim pid=%d state=0x%x", victim->pid, READ_ONCE(victim->__state));

  set_tsk_thread_flag(victim, TIF_SIGPENDING);
  smp_wmb();
  phase = 1;

  kstep_tick_repeat(50);

  int count = atomic_read(stale_count);
  TRACE_INFO("phase=%d victim_state=0x%x stale_count=%d",
             phase, READ_ONCE(victim->__state), count);

  if (count > 0)
    kstep_fail("stale prev_state detected %d times in try_to_block_task", count);
  else
    kstep_pass("no stale prev_state (stale_count=%d)", count);

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
