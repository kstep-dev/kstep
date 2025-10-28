// https://github.com/torvalds/linux/commit/cd9626e9ebc77edec33023fe95dab4b04ffc819d

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
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  send_sigcode(busy_task, SIGCODE_FORK, 5);
}

static void controller_body(void) {
  struct task_struct *pause_task = NULL;

  // find ineligible task
  while (pause_task == NULL) {
    pause_task = find_not_eligible_task(TARGET_TASK, busy_task);
    if (pause_task) {
      break;
    }
    controller_tick();
  }

  // dequeue ineligible task
  TRACE_INFO("dequeue ineligible task %d", pause_task->pid);
  send_sigcode(pause_task, SIGCODE_PAUSE, 0);
  controller_tick();

  // freeze ineligible task
  TRACE_INFO("freeze ineligible task %d", pause_task->pid);
  static_branch_inc(&freezer_active);
  *ksym.pm_freezing = true;
  pause_task->__state |= TASK_FREEZABLE;
  pause_task->__state |= TASK_INTERRUPTIBLE;
  ksym.freeze_task(pause_task);

  controller_tick();
  *ksym.pm_freezing = false;
  static_branch_dec(&freezer_active);

  // wait for 2 ticks
  controller_tick();
  controller_tick();

  // wake up ineligible task
  TRACE_INFO("wake up ineligible task %d", pause_task->pid);
  int ret = ksym.try_to_wake_up(pause_task, TASK_NORMAL, 0);
  if (ret == 0) {
    TRACE_INFO("PASS: try_to_wake_up returned 0");
  } else {
    TRACE_ERR("FAIL: try_to_wake_up returned %d", ret);
  }
}

struct controller_ops controller_cd9626e = {
    .name = "cd9626e",
    .init = controller_init,
    .body = controller_body,
};
