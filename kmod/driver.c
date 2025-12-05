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
  for (int i = 0; i < ARRAY_SIZE(drivers); i++) {
    if (strcmp(drivers[i]->name, name) == 0) {
      return drivers[i];
    }
  }
  panic("Driver %s not found", name);
}

static struct task_struct *tasks[10];
static void noop_init(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void noop_body(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++) 
    kstep_task_wakeup(tasks[i]);

  for (int i = 0; i < 15; i++) 
    kstep_tick();
}

struct kstep_driver noop = {
    .name = "noop",
    .init = noop_init,
    .body = noop_body,
};
