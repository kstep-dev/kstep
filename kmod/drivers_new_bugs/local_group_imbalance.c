// Replay driver for data/fuzz/crashes/work conserving/work conserving_20260325_201209_w0
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
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_CREATE, 0, 0, 0},    // create task 0
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_WAKEUP, 0, 0, 0},    // wake task 0
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_FORK, 0, 1, 0},      // fork task 0 into slot 1
    {OP_TASK_PAUSE, 1, 0, 0},     // pause task 1
    {OP_TASK_CFS, 0, 0, 0},       // switch task 0 to CFS
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_WAKEUP, 1, 0, 0},    // wake task 1
    {OP_TASK_PAUSE, 1, 0, 0},     // pause task 1
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_PAUSE, 0, 0, 0},     // pause task 0
    {OP_TASK_WAKEUP, 0, 0, 0},    // wake task 0
    {OP_TASK_WAKEUP, 1, 0, 0},    // wake task 1
    {OP_TASK_CFS, 1, 0, 0},       // switch task 1 to CFS
    {OP_TASK_CREATE, 2, 0, 0},    // create task 2
    {OP_TASK_WAKEUP, 2, 0, 0},    // wake task 2
    {OP_TASK_PAUSE, 1, 0, 0},     // pause task 1
    {OP_TASK_WAKEUP, 1, 0, 0},    // wake task 1
    {OP_TASK_FORK, 2, 3, 0},      // fork task 2 into slot 3
    {OP_TASK_PIN, 2, 1, 2},       // pin task 2 to CPUs 1-2
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_CREATE, 4, 0, 0},    // create task 4
    {OP_TASK_PAUSE, 3, 0, 0},     // pause task 3
    {OP_TASK_SET_PRIO, 1, -1, 0}, // set task 1 nice level to -1
    {OP_TASK_SET_PRIO, 1, 17, 0}, // set task 1 nice level to 17
    {OP_TASK_CFS, 2, 0, 0},       // switch task 2 to CFS
    {OP_TASK_FORK, 1, 5, 0},      // fork task 1 into slot 5
    {OP_TASK_FORK, 0, 6, 0},      // fork task 0 into slot 6
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_PAUSE, 2, 0, 0},     // pause task 2
    {OP_TASK_WAKEUP, 2, 0, 0},    // wake task 2
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_CREATE, 7, 0, 0},    // create task 7
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_WAKEUP, 4, 0, 0},    // wake task 4
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_FORK, 2, 8, 0},      // fork task 2 into slot 8
    {OP_TASK_WAKEUP, 7, 0, 0},    // wake task 7
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_CFS, 8, 0, 0},       // switch task 8 to CFS
    {OP_TASK_WAKEUP, 3, 0, 0},    // wake task 3
    {OP_TASK_CREATE, 9, 0, 0},    // create task 9
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_PIN, 1, 4, 4},       // pin task 1 to CPU 4
    {OP_CGROUP_CREATE, -1, 0, 0}, // create root cgroup cg0
    {OP_TASK_SET_PRIO, 2, 0, 0},  // set task 2 nice level to 0
    {OP_CGROUP_ADD_TASK, 0, 3, 0},// move task 3 into cgroup cg0
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_WAKEUP, 9, 0, 0},    // wake task 9
    {OP_TASK_PAUSE, 5, 0, 0},     // pause task 5
    {OP_TASK_CFS, 1, 0, 0},       // switch task 1 to CFS
    {OP_TASK_SET_PRIO, 6, -14, 0},// set task 6 nice level to -14
    {OP_TASK_WAKEUP, 5, 0, 0},    // wake task 5
    {OP_TASK_PAUSE, 4, 0, 0},     // pause task 4
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_WAKEUP, 4, 0, 0},    // wake task 4
    {OP_CGROUP_CREATE, -1, 1, 0}, // create root cgroup cg1
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_CGROUP_ADD_TASK, 0, 4, 0},// move task 4 into cgroup cg0
    {OP_TASK_SET_PRIO, 3, -9, 0}, // set task 3 nice level to -9
    {OP_TASK_PIN, 5, 4, 4},       // pin task 5 to CPU 4
    {OP_TASK_CFS, 9, 0, 0},       // switch task 9 to CFS
    {OP_TASK_PAUSE, 3, 0, 0},     // pause task 3
    {OP_TASK_PIN, 6, 4, 4},       // pin task 6 to CPU 4
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
    {OP_TASK_WAKEUP, 3, 0, 0},    // wake task 3
    {OP_TICK, 0, 0, 0},           // advance one scheduler tick
};

static void setup(void) {
  kstep_topo_init();
  {
    const char *cls[] = {"0", "1-2", "1-2", "3-4", "3-4"};
    kstep_topo_set_cls(cls, ARRAY_SIZE(cls));
  }
  kstep_topo_apply();
  kstep_cov_init();
}

static void run(void) {
  int i;
  bool broken;

  TRACE_INFO("Replaying %zu ops from work conserving_20260325_201209_w0",
             ARRAY_SIZE(ops));

  for (i = 0; i < ARRAY_SIZE(ops); i++) {
    while (!kstep_execute_op(ops[i].type, ops[i].a, ops[i].b, ops[i].c)) {
      continue;
    }
  }

  kstep_tick_repeat(1000);
  broken = kstep_work_conserving_broken();
  TRACE_INFO("work_conserving_broken=%d", broken);
  if (broken)
    kstep_fail("work-conserving invariant violated after recorded replay");
  else
    kstep_pass("recorded replay completed without work-conserving violation");
}

KSTEP_DRIVER_DEFINE{
    .name = "local_group_imbalance",
    .setup = setup,
    .run = run,
    .on_tick_end = kstep_output_nr_running,
    .step_interval_us = 10000,
};
