#include "kstep.h"

extern struct kstep_driver even_idle_cpu;
extern struct kstep_driver extra_balance;
extern struct kstep_driver freeze;
extern struct kstep_driver lag_vruntime;
extern struct kstep_driver long_balance;
extern struct kstep_driver sync_wakeup;
extern struct kstep_driver util_avg;
extern struct kstep_driver vruntime_overflow;
extern struct kstep_driver case_time_sensitive;
extern struct kstep_driver noop;

static struct kstep_driver *drivers[] = {
    &even_idle_cpu,       &extra_balance, &freeze,   &lag_vruntime,
    &long_balance,        &sync_wakeup,   &util_avg, &vruntime_overflow,
    &case_time_sensitive, &noop,
};

struct kstep_driver *kstep_driver_get(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(drivers); i++)
    if (strcmp(drivers[i]->name, name) == 0)
      return drivers[i];
  panic("Driver %s not found", name);
}
