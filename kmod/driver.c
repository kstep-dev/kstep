#include "kstep.h"

extern struct kstep_driver default_driver;
extern struct kstep_driver even_idle_cpu;
extern struct kstep_driver extra_balance;
extern struct kstep_driver freeze;
extern struct kstep_driver lag_vruntime;
extern struct kstep_driver long_balance;
extern struct kstep_driver sync_wakeup;
extern struct kstep_driver util_avg;
extern struct kstep_driver vruntime_overflow;
extern struct kstep_driver case_time_sensitive;

static struct kstep_driver *drivers[] = {
    &default_driver,    &even_idle_cpu,       &extra_balance, &freeze,
    &lag_vruntime,      &long_balance,        &sync_wakeup,   &util_avg,
    &vruntime_overflow, &case_time_sensitive,
};

struct kstep_driver *kstep_driver_get(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(drivers); i++)
    if (strcmp(drivers[i]->name, name) == 0)
      return drivers[i];
  panic("Driver %s not found", name);
}

void kstep_driver_print(struct kstep_driver *driver) {
  TRACE_INFO("- %-20s: %s", "name", driver->name);
  TRACE_INFO("- %-20s: %llu", "step_interval_us", driver->step_interval_us);
  TRACE_INFO("- %-20s: %d", "print_rq", driver->print_rq);
  TRACE_INFO("- %-20s: %d", "print_tasks", driver->print_tasks);
  TRACE_INFO("- %-20s: %d", "print_nr_running", driver->print_nr_running);
  TRACE_INFO("- %-20s: %d", "print_load_balance", driver->print_load_balance);
  TRACE_INFO("- %-20s: %d", "print_sched_softirq", driver->print_sched_softirq);
}
