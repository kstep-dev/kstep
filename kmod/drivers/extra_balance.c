// https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3

#include "driver.h"
#include "internal.h" // cpu_rq

static struct task_struct *tasks[5];

static void setup(void) {
  // SMT pairs: [1,2] [3,4], MC: [1-4]
  kstep_topo_init();
  const char *smt[] = {"0", "1-2", "1-2", "3-4", "3-4"};
  kstep_topo_set_level(KSTEP_TOPO_SMT, smt, 5);
  kstep_topo_apply();

  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  // nr_running on cpu 1-4: [1, 0, 3, 1]
  kstep_task_pin(tasks[0], 1, 1);
  kstep_task_pin(tasks[1], 3, 3);
  kstep_task_pin(tasks[2], 3, 3);
  kstep_task_pin(tasks[3], 3, 3);
  kstep_task_pin(tasks[4], 4, 4);

  kstep_tick_repeat(500);
  for (int i = 1; i <= 3; i++)
    kstep_task_pin(tasks[i], 1, 3);
  kstep_tick_repeat(500);
}

static void on_tick_begin(void) {
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "nr_running");
  kstep_json_field_u64(&json, "cpu1", cpu_rq(1)->nr_running);
  kstep_json_field_u64(&json, "cpu2", cpu_rq(2)->nr_running);
  kstep_json_field_u64(&json, "cpu3", cpu_rq(3)->nr_running);
  kstep_json_field_u64(&json, "cpu4", cpu_rq(4)->nr_running);
  kstep_json_end(&json);
}

KSTEP_DRIVER_DEFINE{
    .name = "extra_balance",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .on_sched_balance_selected = kstep_output_balance,
    .step_interval_us = 1000,
};
