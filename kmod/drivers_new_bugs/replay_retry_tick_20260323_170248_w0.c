// Replay driver for data/fuzz/crashes/retry_tick/retry_tick_20260323_170248_w0
// On master, the fuzz replay stalls after 50 retry ticks because TASK_SET_PRIO
// task 8 and task 5 has same nice value, but task 5 get 9 times more cpu time.
#include "driver.h"
#include "internal.h"
#include "op_handler.h"

struct replay_op {
  enum kstep_op_type type;
  int a;
  int b;
  int c;
};

static const struct replay_op ops[] = {
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_CREATE, 0, 0, 0},           // create task 0
    {OP_TASK_WAKEUP, 0, 0, 0},           // wake task 0
    {OP_TASK_SET_PRIO, 0, 19, 0},        // set task 0 nice level to 19
    {OP_TASK_CFS, 0, 0, 0},              // switch task 0 to CFS
    {OP_TASK_CREATE, 1, 0, 0},           // create task 1
    {OP_TASK_SET_PRIO, 0, -1, 0},        // set task 0 nice level to -1
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_CREATE, -1, 0, 0},        // create root cgroup cg0
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_CREATE, 2, 0, 0},           // create task 2
    {OP_TASK_CREATE, 3, 0, 0},           // create task 3
    {OP_TASK_PAUSE, 0, 0, 0},            // pause task 0
    {OP_CGROUP_CREATE, 0, 1, 0},         // create child cgroup cg0/cg1
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_WAKEUP, 0, 0, 0},           // wake task 0
    {OP_TASK_PAUSE, 0, 0, 0},            // pause task 0
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_WAKEUP, 2, 0, 0},           // wake task 2
    {OP_TASK_PIN, 2, 1, 2},              // pin task 2 to CPUs 1-2
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_WAKEUP, 0, 0, 0},           // wake task 0
    {OP_CGROUP_SET_WEIGHT, 0, 8769, 0},  // set cg0 weight to 8769
    {OP_TASK_PAUSE, 0, 0, 0},            // pause task 0
    {OP_TASK_WAKEUP, 1, 0, 0},           // wake task 1
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_SET_CPUSET, 1, 2, 2},     // restrict cg1 to CPUs 2-2
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_WAKEUP, 3, 0, 0},           // wake task 3
    {OP_CGROUP_ADD_TASK, 1, 3, 0},       // move task 3 into cgroup cg1
    {OP_TASK_CREATE, 4, 0, 0},           // create task 4
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_PAUSE, 3, 0, 0},            // pause task 3
    {OP_TASK_WAKEUP, 3, 0, 0},           // wake task 3
    {OP_TASK_SET_PRIO, 2, 17, 0},        // set task 2 nice level to 17
    {OP_TASK_CFS, 1, 0, 0},              // switch task 1 to CFS
    {OP_TASK_PAUSE, 1, 0, 0},            // pause task 1
    {OP_CGROUP_CREATE, -1, 2, 0},        // create root cgroup cg2
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_SET_WEIGHT, 1, 8726, 0},  // set cg1 weight to 8726
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_SET_WEIGHT, 2, 2408, 0},  // set cg2 weight to 2408
    {OP_TASK_WAKEUP, 0, 0, 0},           // wake task 0
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_CREATE, 5, 0, 0},           // create task 5
    {OP_CGROUP_ADD_TASK, 2, 3, 0},       // move task 3 into cgroup cg2
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_FORK, 2, 6, 0},             // fork task 2 into slot 6
    {OP_TASK_WAKEUP, 4, 0, 0},           // wake task 4
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_ADD_TASK, 2, 6, 0},       // move task 6 into cgroup cg2
    {OP_TASK_WAKEUP, 5, 0, 0},           // wake task 5
    {OP_TASK_PAUSE, 4, 0, 0},            // pause task 4
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_WAKEUP, 4, 0, 0},           // wake task 4
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_CREATE, 0, 3, 0},         // create child cgroup cg0/cg3
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_FORK, 4, 7, 0},             // fork task 4 into slot 7
    {OP_TASK_FORK, 3, 8, 0},             // fork task 3 into slot 8
    {OP_CGROUP_SET_CPUSET, 2, 1, 2},     // restrict cg2 to CPUs 1-2
    {OP_TASK_FORK, 3, 9, 0},             // fork task 3 into slot 9
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_SET_CPUSET, 0, 2, 2},     // restrict cg0 to CPUs 2-2
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_ADD_TASK, 2, 7, 0},       // move task 7 into cgroup cg2
    {OP_CGROUP_SET_WEIGHT, 2, 1021, 0},  // set cg2 weight to 1021
    {OP_CGROUP_ADD_TASK, 1, 1, 0},       // move task 1 into cgroup cg1
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_CGROUP_ADD_TASK, 3, 0, 0},       // move task 0 into cgroup cg3
    {OP_CGROUP_CREATE, 2, 4, 0},         // create child cgroup cg2/cg4
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_SET_PRIO, 5, 0, 0},         // set task 5 nice level to 0
    {OP_CGROUP_ADD_TASK, 1, 3, 0},       // move task 3 into cgroup cg1
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_WAKEUP, 1, 0, 0},           // wake task 1
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_PAUSE, 4, 0, 0},            // pause task 4
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_CFS, 3, 0, 0},              // switch task 3 to CFS
    {OP_TICK, 0, 0, 0},                  // advance one scheduler tick
    {OP_TASK_SET_PRIO, 7, -14, 0},       // set task 7 nice level to -14
    {OP_TASK_WAKEUP, 4, 0, 0},           // wake task 4
    {OP_TASK_PAUSE, 7, 0, 0},            // pause task 7
    {OP_CGROUP_ADD_TASK, 1, 9, 0},       // move task 9 into cgroup cg1
    {OP_CGROUP_ADD_TASK, 4, 4, 0},       // move task 4 into cgroup cg4
    {OP_CGROUP_CREATE, -1, 5, 0},        // create root cgroup cg5
};

static void setup(void) { kstep_cov_init(); }

static void run(void) {
  int i;
  // bool ok;

  TRACE_INFO("Replaying %zu ops from retry_tick_20260323_170248_w0",
             ARRAY_SIZE(ops));
  TRACE_INFO("Expected issue: task 0 stays runnable after the recorded retry ticks");
  for (i = 0; i < ARRAY_SIZE(ops); i++) {
    while (!kstep_execute_op(ops[i].type, ops[i].a, ops[i].b, ops[i].c)) {
      kstep_execute_op(OP_TICK, 0, 0, 0);
    }
  }

  for (int i = 0; i < 5000; i++) {
    kstep_execute_op(OP_TICK, 0, 0, 0);
  }

}

KSTEP_DRIVER_DEFINE{
    .name = "replay_retry_tick_170248_w0",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_nr_running,
    .step_interval_us = 1000,
};
