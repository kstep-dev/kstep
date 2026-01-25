#include "internal.h"

extern struct kstep_driver default_driver;
extern struct kstep_driver throttled_limbo_list;
extern struct kstep_driver even_idle_cpu;
extern struct kstep_driver extra_balance;
extern struct kstep_driver freeze;
extern struct kstep_driver h_nr_runnable;
extern struct kstep_driver lag_vruntime;
extern struct kstep_driver long_balance;
extern struct kstep_driver rt_runtime_toggle;
extern struct kstep_driver sync_wakeup;
extern struct kstep_driver time_sensitive;
extern struct kstep_driver uclamp_inversion;
extern struct kstep_driver util_avg;
extern struct kstep_driver vlag_overflow;
extern struct kstep_driver vruntime_overflow;

static struct kstep_driver *drivers[] = {
    &default_driver,
    &throttled_limbo_list,
    &even_idle_cpu,
    &extra_balance,
    &freeze,
    &h_nr_runnable,
    &lag_vruntime,
    &long_balance,
    &rt_runtime_toggle,
    &sync_wakeup,
    &time_sensitive,
    &uclamp_inversion,
    &util_avg,
    &vlag_overflow,
    &vruntime_overflow,
};

struct kstep_driver *kstep_driver_get(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(drivers); i++)
    if (strcmp(drivers[i]->name, name) == 0)
      return drivers[i];
  panic("Driver %s not found", name);
}
