// https://github.com/torvalds/linux/commit/2feab2492deb2f14f9675dd6388e9e2bf669c27a

#include <linux/sched/signal.h>

#include "driver.h"
#include "internal.h"

#define NUM_TASKS 20000

static DEFINE_PER_CPU(ktime_t, sched_softirq_starttime) = 0;

static void sched_softirq_entry(void *ignore, unsigned int vec_nr) {
  if (vec_nr != SCHED_SOFTIRQ || smp_processor_id() == 0)
    return;
  this_cpu_write(sched_softirq_starttime, ktime_get());
}

static void sched_softirq_exit(void *ignore, unsigned int vec_nr) {
  if (vec_nr != SCHED_SOFTIRQ || smp_processor_id() == 0)
    return;

  ktime_t endtime = ktime_get();
  ktime_t starttime = this_cpu_read(sched_softirq_starttime);
  if (!starttime)
    panic("sched_softirq_starttime is not set");
  this_cpu_write(sched_softirq_starttime, 0);
  u64 lat_ns = ktime_to_ns(ktime_sub(endtime, starttime));
  pr_info("sched_softirq: {\"cpu\": %d, \"lat_us\": %llu.%03llu}\n",
          smp_processor_id(), lat_ns / 1000, lat_ns % 1000);
}

static void trace_sched_softirq(void) {
  KSYM_IMPORT_TYPED(struct tracepoint, __tracepoint_softirq_entry);
  KSYM_IMPORT_TYPED(struct tracepoint, __tracepoint_softirq_exit);
  if (tracepoint_probe_register(KSYM___tracepoint_softirq_entry,
                                sched_softirq_entry, NULL))
    panic("Failed to register softirq_entry tracepoint");

  if (tracepoint_probe_register(KSYM___tracepoint_softirq_exit,
                                sched_softirq_exit, NULL))
    panic("Failed to register softirq_exit tracepoint");

  TRACE_INFO("Tracing sched_softirq latency");
}

static struct task_struct *busy_task;

static void setup(void) {
  trace_sched_softirq();
  busy_task = kstep_task_create();
}

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
  kstep_task_pin(busy_task, 1, 1);
  kstep_task_fork(busy_task, NUM_TASKS);
  kstep_sleep_until(fork_finished);
  kstep_tick_repeat(3000);
}

KSTEP_DRIVER_DEFINE{
    .name = "long_balance",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
