// https://github.com/torvalds/linux/commit/eaf5a92ebde5
// Bug: uclamp_fork() incorrectly gives RT-level uclamp.min (1024) to a
// reset-on-fork child that is about to be demoted to SCHED_NORMAL.

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 7, 0)

static struct task_struct *parent;

static void setup(void) {
  parent = kstep_task_create();
  kstep_task_pin(parent, 1, 1);
}

static void run(void) {
  // Set parent to SCHED_FIFO (RT)
  kstep_task_fifo(parent);

  TRACE_INFO("parent pid=%d policy=%d rt_priority=%d",
             parent->pid, parent->policy, parent->rt_priority);

  // Set reset-on-fork flag on the parent
  parent->sched_reset_on_fork = 1;

  TRACE_INFO("parent sched_reset_on_fork=%d uclamp_req[MIN].value=%u",
             parent->sched_reset_on_fork,
             parent->uclamp_req[UCLAMP_MIN].value);

  // Fork the parent to create a child
  kstep_task_fork(parent, 1);

  // Let the child settle
  kstep_tick_repeat(3);

  // Find the child task from the parent's children list
  struct task_struct *child = NULL;
  struct task_struct *c;
  list_for_each_entry(c, &parent->children, sibling) {
    child = c;
    break;
  }

  if (!child) {
    kstep_fail("no child found after fork");
    return;
  }

  unsigned int child_uclamp_min = child->uclamp_req[UCLAMP_MIN].value;
  unsigned int child_uclamp_max = child->uclamp_req[UCLAMP_MAX].value;
  int child_policy = child->policy;
  int child_reset = child->sched_reset_on_fork;

  TRACE_INFO("child pid=%d policy=%d reset_on_fork=%d",
             child->pid, child_policy, child_reset);
  TRACE_INFO("child uclamp_req[MIN]=%u uclamp_req[MAX]=%u",
             child_uclamp_min, child_uclamp_max);

  // After reset-on-fork, child should be SCHED_NORMAL (policy 0)
  if (child_policy != SCHED_NORMAL) {
    kstep_fail("child policy=%d, expected SCHED_NORMAL(0)", child_policy);
    return;
  }

  // Bug: child gets uclamp.min=1024 from RT parent despite being SCHED_NORMAL
  // Fixed: child gets uclamp.min=0 (default for UCLAMP_MIN)
  if (child_uclamp_min == 1024) {
    kstep_fail("child has uclamp_req[MIN]=%u (RT boost leaked to SCHED_NORMAL child)",
               child_uclamp_min);
  } else if (child_uclamp_min == 0) {
    kstep_pass("child has correct uclamp_req[MIN]=%u", child_uclamp_min);
  } else {
    kstep_fail("child has unexpected uclamp_req[MIN]=%u", child_uclamp_min);
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "resetonfork_rt_uclamp",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "resetonfork_rt_uclamp",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
