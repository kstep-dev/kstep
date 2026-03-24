// On v6.18, the scheduler fails to rebalance the CFS task away from the CPU with a running FIFO task
// On master, it works well
#include "driver.h"
#include "internal.h"
#include "op_handler.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
struct replay_op {
  enum kstep_op_type type;
  int a;
  int b;
  int c;
};

static const struct replay_op ops[] = {
    {OP_TASK_CREATE, 0, 0, 0},       // create task 0
    {OP_TASK_WAKEUP, 0, 0, 0},       // wake task 0
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TASK_PAUSE, 0, 0, 0},        // pause task 0
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TASK_WAKEUP, 0, 0, 0},       // wake task 0 again
    {OP_TASK_CREATE, 1, 0, 0},       // create task 1
    {OP_TASK_WAKEUP, 1, 0, 0},       // wake task 1
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TASK_PAUSE, 0, 0, 0},        // pause task 0
    {OP_CGROUP_CREATE, -1, 0, 0},    // create root cgroup cg0
    {OP_TASK_FORK, 1, 2, 0},         // fork task 1 into slot 2
    {OP_CGROUP_CREATE, -1, 1, 0},    // create root cgroup cg1
    {OP_TASK_CREATE, 3, 0, 0},       // create task 3
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TASK_SET_PRIO, 1, 19, 0},    // set task 1 nice level to 19
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TASK_FIFO, 1, 0, 0},         // switch task 1 to SCHED_FIFO
    {OP_TASK_PIN, 1, 2, 2},          // pin task 1 to CPU 2
    {OP_TASK_FORK, 1, 4, 0},         // fork task 1 into slot 4
    {OP_TASK_WAKEUP, 0, 0, 0},       // wake task 0
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TASK_CFS, 0, 0, 0},          // switch task 0 back to CFS
    {OP_TASK_PAUSE, 1, 0, 0},        // pause task 1
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TASK_WAKEUP, 3, 0, 0},       // wake task 3
    {OP_TASK_CREATE, 5, 0, 0},       // create task 5
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TICK, 0, 0, 0},              // advance one scheduler tick
    {OP_TASK_PAUSE, 4, 0, 0},        // pause task 4
    {OP_CGROUP_ADD_TASK, 1, 0, 0},   // move task 0 into cgroup cg1
    {OP_CGROUP_SET_CPUSET, 1, 1, 2}, // restrict cg1 to CPUs 1-2
    {OP_TASK_WAKEUP, 4, 0, 0},       // wake task 4
    {OP_CGROUP_CREATE, 0, 2, 0},     // create child cgroup cg0/cg2
    {OP_TASK_CREATE, 6, 0, 0},       // create task 6
};

static void setup(void) { kstep_cov_init(); }

static void run(void) {
  int i;

  TRACE_INFO("Replaying %zu ops from retry_tick_20260321_214311_w0",
             ARRAY_SIZE(ops));
  for (i = 0; i < ARRAY_SIZE(ops); i++)
    kstep_execute_op(ops[i].type, ops[i].a, ops[i].b, ops[i].c);
  for (i = 0; i < 5000; i++) {
    kstep_execute_op(OP_TICK, 0, 0, 0);
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "replay_retry_tick",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_nr_running,
    .step_interval_us = 1000,
};
#endif