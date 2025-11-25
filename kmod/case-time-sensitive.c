#include <linux/random.h>

#include "kstep.h"

#define TARGET_TASK "test-proc"

// ./run_qemu.py --params controller=case_time_sensitive --log_file data/logs/caseSensitiveTime.log
// ./plot/plot_nr_running_queued.py --log data/logs/caseSensitiveTime.log --cpus 1 --time-start 10 --output plot/case_time_sensitive.pdf
static struct task_struct *busy_task;

static void controller_init(void) {
  kstep_sleep();

  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
}

static struct task_struct *find_target_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task || p->on_cpu == 0)
      continue;
    return p;
  }
  return NULL;
}

static void controller_body(void) {
  // fork 3 processes on cpu 1
  send_sigcode2(busy_task, SIGCODE_FORK_PIN, 4, 2);

  // The pattern: Make nr_queued > nr_running, and then run until nr_queued == nr_running before ending the "highlight".
  for (int i = 0; i < 10; i++) {
    int rand_iter = 10 + (get_random_u32() % 11); // 10 to 20 inclusive

    // Step 1: Let the system advance randomly, building up some queued work
    for (int j = 0; j < rand_iter; j++) {
      kstep_tick();
    }

    // Step 2: Pause a newly-forked task, which will result in it being queued but not running
    struct task_struct *target_task = find_target_task();
    send_sigcode(target_task, SIGCODE_PAUSE, 0);

    // Step 3: Wait until nr_queued == nr_running again, i.e. the queue clears (simulate shade end)
    // For the workload's purposes, we keep calling tick until that's the case.
    // (We can't literally query nr_running/nr_queued, but we can simulate "work until queue clears")

    // Give it a fixed window where queue likely > running
    for (int j = 0; j < 15; j++) {
      kstep_tick();
    }

    // Now, simulate a draining phase: unpause the "queued" task if possible -- here, we resume the busy task
    send_sigcode2(busy_task, SIGCODE_FORK_PIN, 1, 2);
    // (or you might send a SIGCODE_RESUME to "target_task" if that makes more sense)
  }
}

struct controller_ops controller_case_time_sensitive = {
    .name = "case_time_sensitive",
    .init = controller_init,
    .body = controller_body,
};
