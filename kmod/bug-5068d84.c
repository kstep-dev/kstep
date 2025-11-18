#include "kstep.h"

#define TARGET_TASK "test-proc"
#define CGROUP_TASK "cgroup-proc"

static struct task_struct *busy_task;
static struct task_struct *cgroup_task;

static void controller_init(void) {
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  kstep_sleep();
  cgroup_task = poll_task(CGROUP_TASK);

  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 0);
  send_sigcode2(cgroup_task, SIGCODE_SETCPU_CGROUP, 1 << 16 | 0x0, 2);
}

static struct task_struct *find_last_task(void) {
    struct task_struct *p;
    struct task_struct *target;
    for_each_process(p) {
        if (strcmp(p->comm, TARGET_TASK) != 0)
          continue;
        target = p;
    }
    return target;
}

static void controller_body(void) {
    for (int i = 0; i < 3; i++) {
        send_sigcode2(busy_task, SIGCODE_FORK_PIN, 1, 2);
        struct task_struct *pin_task = find_last_task();
        send_sigcode(pin_task, SIGCODE_REWEIGHT, 19);
    }
    
    send_sigcode(busy_task, SIGCODE_CLONE3_L1_0, 1);
    struct task_struct *l1_0_task = find_last_task();

    for(int i = 0; i < 20; i++) {
        call_tick_once(true);
    }

    send_sigcode2(cgroup_task, SIGCODE_REWEIGHT_CGROUP, 1 << 16 | 0x0, 10000);

    for(int i = 0; i < 10; i++) {
        call_tick_once(true);
    }
    print_tasks();

}

struct controller_ops controller_5068d84 = {
    .name = "5068d84",
    .init = controller_init,
    .body = controller_body,
};
