// https://github.com/torvalds/linux/commit/2feab2492deb2f14f9675dd6388e9e2bf669c27a

#include <linux/sched/signal.h>

#include "driver.h"

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

// All on CPU 1: the point is one runqueue long enough that walking it in the balancer's
// softirq takes measurable time. Only the queue length matters, so they are plain runnable
// tasks and the driver never addresses them individually. Created here rather than in run()
// because each create waits for its task to reach its first read, which needs the real timer
// that kstep_tick_init() turns off between setup() and run() (see main.c).
static void setup(void) {
  kstep_on_softirq_begin(on_sched_softirq_begin);
  kstep_on_softirq_end(on_sched_softirq_end);

  for (int i = 0; i < NUM_TASKS; i++) {
    struct task_struct *p = kstep_task_create();

    kstep_task_pin(p, 1, 1);
    kstep_task_wakeup(p);
  }
}

static void run(void) {
  kstep_tick_repeat(2000);
}

KSTEP_DRIVER_DEFINE{
    .name = "long_balance",
    .setup = setup,
    .run = run,
};
