#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/kthread.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "utils.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;

static void controller_init(void) {
  // set_cpus_allowed_ptr(busy_kthread, &mask);
  udelay(SIM_INTERVAL_US);

  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);

  send_sigcode(busy_task, SIGCODE_FORK, 3);
  udelay(SIM_INTERVAL_US);
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

static void controller_body(void) {

  for (int i = 0; i < 20; i++) {
    call_tick_once();
  }

  struct task_struct *pause_task = NULL;

  while (1) {
    pause_task = find_not_eligible_task();
    if (pause_task) {
      TRACE_INFO("dequeue ineligible task %d", pause_task->pid);
      send_sigcode(pause_task, SIGCODE_SLEEP, 1);
      break;
    }
    call_tick_once();
  }

  call_tick_once();

  TRACE_INFO("freeze ineligible task %d", pause_task->pid);
  static_branch_inc(&freezer_active);
  *ksym.pm_freezing = true;
  ksym.freeze_task(pause_task);

  *ksym.pm_freezing = false;
  static_branch_dec(&freezer_active);
  udelay(SIM_INTERVAL_US);

  call_tick_once();
  call_tick_once();

  send_sigcode(pause_task, SIGCODE_UNKNOWN, 0);
  print_tasks();
  udelay(SIM_INTERVAL_US);

  for (int i = 0; i < 20; i++) {
    call_tick_once();
  }
}

struct controller_ops controller_cd9626e = {
    .name = "cd9626e",
    .init = controller_init,
    .body = controller_body,
};
