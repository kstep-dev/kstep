// Replay of fuzz_20260406_205141/error/cfs_util_decay/w0_20260406_205539
//
// An RT (fifo) task runs on cpu 1 at half frequency, then alternates short
// sleeps with long busy windows. The brief idle gaps make the rq's util_avg
// jump rather than decay smoothly.

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)

#define WAKE_TICKS 200
#define SLEEP_TICKS 1
#define CYCLES 10

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
  // fake the frequency of cpu 1 to 50% of the base frequency
  kstep_freq_set("1=512");
}

static void run(void) {
  // start the fifo task and let it run for a long busy window
  kstep_task_fifo(task);
  kstep_task_wakeup(task);
  kstep_tick_repeat(WAKE_TICKS);

  // alternate brief sleeps with long busy windows to drive the util_avg jump
  for (int i = 0; i < CYCLES; i++) {
    kstep_task_pause(task);
    kstep_tick_repeat(SLEEP_TICKS);
    kstep_task_wakeup(task);
    if (i < CYCLES - 1)
      kstep_tick_repeat(WAKE_TICKS);
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
