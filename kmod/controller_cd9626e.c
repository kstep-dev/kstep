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

static int controller_init(void) {
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  send_sigcode(busy_task, SIGCODE_FORK, 5);
  return 0;
}

enum state {
  STATE_INIT,
  STATE_PAUSED,
  STATE_WAKING,
  STATE_DONE,
};
static enum state state = STATE_INIT;
static struct task_struct *pause_task;

static int controller_step(int iter) {
  if (state == STATE_INIT) {
    struct task_struct *p = find_not_eligible_task(TARGET_TASK, busy_task);
    if (!p)
      return 0;
    pause_task = p;

    TRACE_INFO("dequeue ineligible task %d", pause_task->pid);
    send_sigcode(pause_task, SIGCODE_PAUSE, 0);

    msleep(SIM_INTERVAL_MS);
    state = STATE_PAUSED;
  }

  else if (state == STATE_PAUSED) {
    TRACE_INFO("freeze ineligible task %d", pause_task->pid);
    static_branch_inc(&freezer_active);
    *ksym.pm_freezing = true;
    pause_task->__state |= TASK_FREEZABLE;
    pause_task->__state |= TASK_INTERRUPTIBLE;
    ksym.freeze_task(pause_task);

    msleep(SIM_INTERVAL_MS);
    *ksym.pm_freezing = false;
    static_branch_dec(&freezer_active);
    state = STATE_WAKING;
  }

  else if (state == STATE_WAKING) {
    TRACE_INFO("wake up ineligible task %d", pause_task->pid);
    ksym.try_to_wake_up(pause_task, TASK_NORMAL, 0);
    msleep(SIM_INTERVAL_MS);
    state = STATE_DONE;
  }

  return state == STATE_DONE;
}

static int controller_exit(void) { return 0; }

struct controller_ops controller_cd9626e = {
    .name = "cd9626e",
    .init = controller_init,
    .step = controller_step,
    .exit = controller_exit,
};
