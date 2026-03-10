// Reproduce: sched: Reduce the default slice to avoid tasks getting an extra tick
// https://github.com/torvalds/linux/commit/2ae891b82695
//
// With HZ=1000 and 8+ CPUs, the base slice scales to 3.0ms (0.75ms * 4).
// Ticks arriving slightly faster than 1ms cause tasks to miss their deadline
// check and get an extra tick, running ~4ms instead of ~3ms.
// The fix reduces the base slice from 0.75ms to 0.70ms (→ 2.8ms), creating
// margin so that 3 ticks (2.997ms) exceed the deadline.

#include "driver.h"
#include "internal.h"

static struct task_struct *tasks[2];

static int tick_count;
static int last_pid;
static int run_length;
static int max_run_length;
static int switch_count;

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void on_tick(void) {
  struct rq *rq = cpu_rq(1);
  struct task_struct *curr = rq->curr;

  tick_count++;

  if (curr->pid == last_pid) {
    run_length++;
  } else {
    if (last_pid != 0) {
      TRACE_INFO("tick %d: task %d ran for %d ticks", tick_count, last_pid,
                 run_length);
      if (run_length > max_run_length)
        max_run_length = run_length;
      switch_count++;
    }
    last_pid = curr->pid;
    run_length = 1;
  }

  kstep_output_curr_task();
}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_pin(tasks[i], 1, 1);

  kstep_tick_repeat(60);

  // Final accounting
  if (run_length > max_run_length)
    max_run_length = run_length;
  TRACE_INFO("Final: task %d ran for %d ticks", last_pid, run_length);
  TRACE_INFO("Max consecutive run length: %d ticks (%d switches)",
             max_run_length, switch_count);

  // Buggy (0.75ms base): slice = 3.0ms, 3 ticks @ 0.999ms = 2.997ms < 3.0ms
  //   → tasks get 4th tick
  // Fixed (0.70ms base): slice = 2.8ms, 3 ticks @ 0.999ms = 2.997ms > 2.8ms
  //   → tasks preempted after 3 ticks
  if (max_run_length >= 4 && switch_count >= 2)
    kstep_fail("Bug: tasks running %d ticks (expected <= 3)", max_run_length);
  else
    kstep_pass("Tasks running %d ticks max (%d switches)", max_run_length,
               switch_count);
}

KSTEP_DRIVER_DEFINE{
    .name = "extra_tick",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick,
    .tick_interval_ns = 999000, // 0.999ms - simulates ticks arriving early
    .step_interval_us = 1000,
};
