// https://github.com/torvalds/linux/commit/2feab2492deb2f14f9675dd6388e9e2bf669c27a

#include <linux/sched/signal.h>

#include "driver.h"
#include "internal.h"

#define NUM_TASKS 20000

static DEFINE_PER_CPU(ktime_t, sched_softirq_starttime) = 0;

static void on_sched_softirq_begin(void) {
  this_cpu_write(sched_softirq_starttime, ktime_get());
}

static void on_sched_softirq_end(void) {
  ktime_t starttime = this_cpu_read(sched_softirq_starttime);
  u64 lat_ns = ktime_to_ns(ktime_sub(ktime_get(), starttime));
  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "type", "sched_softirq");
  kstep_json_field_u64(&json, "cpu", smp_processor_id());
  kstep_json_field_fmt(&json, "lat_us", "%llu.%03llu", lat_ns / 1000,
                       lat_ns % 1000);
  kstep_json_end(&json);
  TRACE_INFO("sched_softirq on CPU %d, latency: %llu.%03llu ms",
             smp_processor_id(), lat_ns / 1000, lat_ns % 1000);
}

static struct task_struct *busy_task;

static void setup(void) { busy_task = kstep_task_create(); }

static void *fork_finished(void) {
  struct task_struct *p;
  int nr_running = 0;
  for_each_process(p) {
    if (task_cpu(p) == task_cpu(busy_task) && p->parent == busy_task)
      nr_running++;
  }
  return nr_running >= NUM_TASKS ? (void *)true : (void *)false;
}

static void run(void) {
  kstep_task_kernel_pin(busy_task, 1, 1);
  kstep_task_kernel_wakeup(busy_task);
  kstep_task_signal_fork(busy_task, NUM_TASKS);
  kstep_sleep_until(fork_finished);
  kstep_tick_repeat(2000);
}

KSTEP_DRIVER_DEFINE{
    .name = "long_balance",
    .setup = setup,
    .run = run,
    .on_sched_softirq_begin = on_sched_softirq_begin,
    .on_sched_softirq_end = on_sched_softirq_end,
    .step_interval_us = 1000,
};
