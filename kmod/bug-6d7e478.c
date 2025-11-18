#include "kstep.h"

#define TARGET_TASK "test-proc"
#define CGROUP_TASK "cgroup-proc"

static struct task_struct *busy_task;
static struct task_struct *cgroup_task;

static void controller_pre_init(void) { kstep_trace_lb(); }

static void controller_init(void) {
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  kstep_sleep();
  send_sigcode(busy_task, SIGCODE_PIN, 1);
  cgroup_task = poll_task(CGROUP_TASK);
}

static void controller_body(void) {
  // making the nr_running on cpu 4-7 to [1, 0, 3, 1]
  send_sigcode(busy_task, SIGCODE_PIN, 4);
  send_sigcode3(busy_task, SIGCODE_FORK_PIN_RANGE, 3, 6, 6);
  send_sigcode3(busy_task, SIGCODE_FORK_PIN_RANGE, 1, 7, 7);
  
  for (int i = 0; i < 1000; i++) {
    call_tick_once(true);
  }

  struct task_struct *pin_task;
  for_each_process(pin_task) {
    if (strcmp(pin_task->comm, TARGET_TASK) != 0 || pin_task == busy_task)
      continue;
    if (task_cpu(pin_task) == 6)
      send_sigcode2(pin_task, SIGCODE_PIN, 4, 6);
  }


  for(int i = 0; i < 1000; i++) {
    call_tick_once(true);
  }

}

// smp 8,sockets=2,cores=2,threads=2
struct controller_ops controller_6d7e478 = {
    .name = "6d7e478",
    .pre_init = controller_pre_init,
    .init = controller_init,
    .body = controller_body,
};
