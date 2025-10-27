#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched_clock.h>
#include <linux/workqueue.h>

// Linux private headers
#include <kernel/sched/sched.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "sigcode.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;
static int done = 0;
static struct task_struct *pause_task = NULL;

static int controller_init(void) {
  busy_task = poll_task(TARGET_TASK);

  busy_task->se.vruntime = INIT_TIME_NS;
  busy_task->nivcsw = 0;
  busy_task->nvcsw = 0;

  send_sigcode(busy_task, SIGCODE_FORK, 5);
  msleep(SIM_INTERVAL_MS);

  return 0;
}

static struct task_struct *find_not_eligible_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task)
      continue;
    if (p->on_cpu == 0)
      continue;
    TRACE_DEBUG("pid=%d, eligible=%d, on_cpu=%d", p->pid,
                ksym.entity_eligible(p->se.cfs_rq, &p->se), p->on_cpu);

    if (ksym.entity_eligible(p->se.cfs_rq, &p->se) == 0) {
      return p;
    }
  }
  return NULL;
}

static int controller_step(int iter) {
  struct task_struct *p = find_not_eligible_task();

  if (p && done == 0) {
    TRACE_INFO("dequeue ineligible task %d", p->pid);
    pause_task = p;
    send_sigcode(pause_task, SIGCODE_PAUSE, 1000);

    msleep(SIM_INTERVAL_MS);
    done = 1;
  }

  if (done == 2 && pause_task != NULL) {
    TRACE_INFO("freeze ineligible task %d", pause_task->pid);
    static_branch_inc(&freezer_active);
    *ksym.pm_freezing = true;
    pause_task->__state |= TASK_FREEZABLE;
    pause_task->__state |= TASK_INTERRUPTIBLE;
    ksym.freeze_task(pause_task);

    msleep(SIM_INTERVAL_MS);
    *ksym.pm_freezing = false;
    static_branch_dec(&freezer_active);
    done = 3;
  }
  if (done == 5 && pause_task != NULL) {
    TRACE_INFO("wake up ineligible task %d", pause_task->pid);
    ksym.try_to_wake_up(pause_task, TASK_NORMAL, 0);
    msleep(SIM_INTERVAL_MS);
    done = 6;
  }

  if (done == 1) {
    done = 2;
  }
  if (done == 3 || done == 4) {
    done++;
  }

  return 0;
}

static int controller_exit(void) { return 0; }

struct controller_ops bug_cd9626e_ops = {
    .name = "cd9626e",
    .init = controller_init,
    .step = controller_step,
    .exit = controller_exit,
};
