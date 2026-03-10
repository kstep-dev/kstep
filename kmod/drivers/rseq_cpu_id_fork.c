// https://github.com/torvalds/linux/commit/ce3614daabea
//
// Bug: __rseq_abi.cpu_id contains incorrect values for newly created tasks
// after fork. The scheduler calls __set_task_cpu() directly in sched_fork()
// and wake_up_new_task(), bypassing rseq_migrate(). This leaves
// rseq_event_mask stale: the RSEQ_EVENT_MIGRATE bit is never set, so
// user-space rseq critical sections see incorrect cpu_id values.
//
// Fix: Add rseq_migrate(p) before __set_task_cpu() in both sched_fork()
// and wake_up_new_task().
//
// Reproduce: Create a user-space task, clear its rseq_event_mask, fork it,
// and check whether the child's rseq_event_mask has the RSEQ_EVENT_MIGRATE
// bit set.  On the buggy kernel it will be 0; on the fixed kernel it will
// have the MIGRATE bit because sched_fork/wake_up_new_task now call
// rseq_migrate().

#include "driver.h"
#include "internal.h"
#include <linux/sched/signal.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

// RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT = 2
#define RSEQ_EVENT_MIGRATE_BIT 2
#define RSEQ_EVENT_MIGRATE_VAL (1UL << RSEQ_EVENT_MIGRATE_BIT)

static struct task_struct *parent;

static void setup(void) {
  parent = kstep_task_create();
  kstep_task_pin(parent, 1, 1);
}

static void *fork_done(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (p->real_parent == parent)
      return p;
  }
  return NULL;
}

static void run(void) {
  kstep_tick_repeat(5);

  // Clear parent's rseq_event_mask so child inherits a clean slate.
  parent->rseq_event_mask = 0;

  TRACE_INFO("parent pid=%d cpu=%d rseq_event_mask=0x%lx rseq=%p",
             parent->pid, task_cpu(parent),
             parent->rseq_event_mask, parent->rseq);

  // Fork once. The child goes through sched_fork() and wake_up_new_task().
  kstep_task_fork(parent, 1);

  // Wait for the child to appear.
  struct task_struct *child = kstep_sleep_until(fork_done);
  if (!child) {
    kstep_fail("could not find child task after fork");
    return;
  }

  unsigned long mask = child->rseq_event_mask;
  int child_cpu = task_cpu(child);

  TRACE_INFO("child pid=%d cpu=%d rseq_event_mask=0x%lx rseq=%p",
             child->pid, child_cpu, mask, child->rseq);

  if (mask & RSEQ_EVENT_MIGRATE_VAL) {
    kstep_pass("child rseq_event_mask has MIGRATE bit (0x%lx): "
               "rseq_migrate called in sched_fork/wake_up_new_task", mask);
  } else {
    kstep_fail("child rseq_event_mask missing MIGRATE bit (0x%lx): "
               "rseq_migrate NOT called in sched_fork/wake_up_new_task", mask);
  }

  kstep_tick_repeat(5);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "rseq_cpu_id_fork",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
