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

static struct task_struct *poll_target_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    TRACE_DEBUG("pid=%d, comm=%s, state=%x, on_cpu=%d", p->pid, p->comm,
                p->__state, p->on_cpu);
  }
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0)
        return p;
    }
    msleep(SIM_INTERVAL_MS);
    TRACE_INFO("Waiting for process %s to be created", TARGET_TASK);
  }
}

static void controller_init(void) {
  // set_cpus_allowed_ptr(busy_kthread, &mask);
  msleep(SIM_INTERVAL_MS);

  busy_task = poll_target_task();
  reset_task_stats(busy_task);

  send_sigcode(busy_task, SIGCODE_FORK, 3);
  msleep(SIM_INTERVAL_MS);
}

int done = 0;

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

static struct task_struct *pause_task = NULL;
static void controller_body(void) {

  for (int i = 0; i < 20; i++) {
    call_tick_once();
  }

  while (1) {
    struct task_struct *p = find_not_eligible_task();
    if (p) {
      TRACE_INFO("dequeue ineligible task %d", p->pid);
      pause_task = p;
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
  msleep(SIM_INTERVAL_MS);

  call_tick_once();
  call_tick_once();

  send_sigcode(pause_task, SIGCODE_UNKNOWN, 0);
  print_tasks();
  msleep(SIM_INTERVAL_MS);

  for (int i = 0; i < 20; i++) {
    call_tick_once();
  }
}

struct controller_ops controller_cd9626e = {
    .name = "cd9626e",
    .init = controller_init,
    .body = controller_body,
};
