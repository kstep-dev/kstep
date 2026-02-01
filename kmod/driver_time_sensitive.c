#include <linux/random.h>

#include "driver.h"

// ./run_qemu.py --params controller=case_time_sensitive --log_file
// data/logs/caseSensitiveTime.log
// ./plot/plot_nr_running_queued.py --log data/logs/caseSensitiveTime.log --cpus
// 1 --time-start 10 --output plot/case_time_sensitive.pdf

static struct task_struct *tasks[3];

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  // pin tasks on cpu 1
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_pin(tasks[i], 1, 1);

  // The pattern: Make nr_queued > nr_running, and then run until nr_queued ==
  // nr_running before ending the "highlight".
  for (int i = 0; i < 10; i++) {
    int rand_iter = 10 + (get_random_u32() % 11); // 10 to 20 inclusive

    // Step 1: Let the system advance randomly, building up some queued work
    for (int j = 0; j < rand_iter; j++) {
      kstep_tick();
    }

    // Step 2: Pause a newly-forked task, which will result in it being queued
    // but not running
    kstep_task_pause(tasks[2]);

    // Step 3: Wait until nr_queued == nr_running again, i.e. the queue clears
    // (simulate shade end) For the workload's purposes, we keep calling tick
    // until that's the case. (We can't literally query nr_running/nr_queued,
    // but we can simulate "work until queue clears")

    // Give it a fixed window where queue likely > running
    for (int j = 0; j < 15; j++) {
      kstep_tick();
    }

    // Now, simulate a draining phase: unpause the "queued" task if possible --
    // here, we resume the busy task
    kstep_task_wakeup(tasks[0]);
    // (or you might send a SIGCODE_RESUME to "target_task" if that makes more
    // sense)
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "time_sensitive",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .print_tasks = true,
    .print_rq = true,
};
