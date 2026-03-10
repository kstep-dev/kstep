// https://github.com/torvalds/linux/commit/5bc78502322a
//
// Bug: exit_mm() clears current->mm without updating rq->membarrier_state,
// leaving the runqueue's membarrier_state stale. This can cause membarrier()
// to skip IPIs to CPUs where an exiting task performed user-space memory
// accesses, violating memory ordering guarantees.
//
// Fix: The fix adds membarrier_update_current_mm(NULL) in exit_mm() to
// synchronize rq->membarrier_state when the mm is cleared.
//
// Observable: After a task with membarrier-registered mm exits, check
// rq->membarrier_state on the CPU where the task ran.
// Buggy: rq->membarrier_state remains stale (non-zero).
// Fixed: rq->membarrier_state is cleared to 0.

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 10, 0)

#include <linux/sched/mm.h>
#include <linux/signal.h>
#include "../user.h"

static struct task_struct *task_a;

static void task_exit(struct task_struct *p) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = SIGCODE_EXIT,
  };
  send_sig_info(SIGUSR1, &info, p);
  kstep_sleep();
}

static void setup(void) {
  task_a = kstep_task_create();
}

static void run(void) {
  int target_cpu = 1;
  struct rq *rq = cpu_rq(target_cpu);

  kstep_task_pin(task_a, target_cpu, target_cpu);
  kstep_task_wakeup(task_a);
  kstep_tick_repeat(5);

  // Verify the task is running on the target CPU
  struct mm_struct *mm = task_a->mm;
  TRACE_INFO("task_a pid=%d cpu=%d mm=%px", task_a->pid,
             task_cpu(task_a), mm);

  if (!mm) {
    kstep_fail("task_a has no mm, cannot test membarrier");
    return;
  }

  // Register membarrier: set MEMBARRIER_STATE_GLOBAL_EXPEDITED on mm
  int state = MEMBARRIER_STATE_GLOBAL_EXPEDITED |
              MEMBARRIER_STATE_GLOBAL_EXPEDITED_READY;
  atomic_set(&mm->membarrier_state, state);

  // Propagate to the runqueue (as ipi_sync_rq_state would do)
  WRITE_ONCE(rq->membarrier_state, state);

  int pre_rq_state = READ_ONCE(rq->membarrier_state);
  TRACE_INFO("before exit: rq[%d].membarrier_state=0x%x mm->membarrier_state=0x%x",
             target_cpu, pre_rq_state, atomic_read(&mm->membarrier_state));

  // Make the task exit, triggering exit_mm()
  task_exit(task_a);

  // Give time for the exit to complete
  kstep_tick_repeat(10);
  kstep_sleep();
  kstep_sleep();

  int post_rq_state = READ_ONCE(rq->membarrier_state);
  TRACE_INFO("after exit: rq[%d].membarrier_state=0x%x task_a->mm=%px",
             target_cpu, post_rq_state, task_a->mm);

  if (post_rq_state != 0) {
    kstep_fail("rq[%d].membarrier_state=0x%x after exit_mm "
               "(stale: not cleared to 0, membarrier may skip IPIs)",
               target_cpu, post_rq_state);
  } else {
    kstep_pass("rq[%d].membarrier_state=0x%x after exit_mm "
               "(correctly cleared by membarrier_update_current_mm)",
               target_cpu, post_rq_state);
  }
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "exit_mm_membarrier",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
