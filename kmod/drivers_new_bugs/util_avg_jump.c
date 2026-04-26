// Replay driver for data/fuzz_20260406_205141/error/cfs_util_decay/w0_20260406_205539
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

#define wake_interval 200
#define sleep_interval 1

static const struct replay_op ops[] = {
  {OP_TASK_CREATE, 0, 0, 0},
  {OP_TASK_WAKEUP, 0, 0, 0},
  {OP_TASK_FIFO, 0, 0, 0},
  {OP_TICK_REPEAT, 200, 0, 0},
  {OP_TASK_PAUSE, 0, 0, 0},
  {OP_TICK_REPEAT, sleep_interval, 0, 0},
  {OP_TASK_WAKEUP, 0, 0, 0},
  {OP_TICK_REPEAT, wake_interval, 0, 0},
  {OP_TASK_PAUSE, 0, 0, 0},
  {OP_TICK_REPEAT, sleep_interval, 0, 0},
  {OP_TASK_WAKEUP, 0, 0, 0},
  {OP_TICK_REPEAT, wake_interval, 0, 0},
  {OP_TASK_PAUSE, 0, 0, 0},
  {OP_TICK_REPEAT, sleep_interval, 0, 0},
  {OP_TASK_WAKEUP, 0, 0, 0},
  {OP_TICK_REPEAT, wake_interval, 0, 0},
  {OP_TASK_PAUSE, 0, 0, 0},
  {OP_TICK_REPEAT, sleep_interval, 0, 0},
  {OP_TASK_WAKEUP, 0, 0, 0},
  {OP_TICK_REPEAT, wake_interval, 0, 0},
  {OP_TASK_PAUSE, 0, 0, 0},
  {OP_TICK_REPEAT, sleep_interval, 0, 0},
  {OP_TASK_WAKEUP, 0, 0, 0},
};


static void setup(void) {
  kstep_cpu_set_freq(1, 512);
  kstep_cov_init();
}

static void run(void) {
  int i;

  TRACE_INFO("Replaying %zu ops from fuzz_20260406_205141/error/cfs_util_decay/w0_20260406_205539",
             ARRAY_SIZE(ops));

  for (i = 0; i < ARRAY_SIZE(ops); i++) {
    while (!kstep_execute_op(ops[i].type, ops[i].a, ops[i].b, ops[i].c))
      continue;
  }
}

static void on_tick_begin(void) {
  struct rq *rq = cpu_rq(1);
  u64 avg_util = rq->avg_rt.util_avg;
  kstep_json_print_2kv("type", "avg_util", "val", "%llu", avg_util);
}

KSTEP_DRIVER_DEFINE{
    .name = "util_avg_jump",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 1000,
};
#endif
