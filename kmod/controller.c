#include "kstep.h"

extern struct controller_ops controller_even_idle_cpu;
extern struct controller_ops controller_extra_balance;
extern struct controller_ops controller_freeze;
extern struct controller_ops controller_lag_vruntime;
extern struct controller_ops controller_long_balance;
extern struct controller_ops controller_sync_wakeup;
extern struct controller_ops controller_util_avg;
extern struct controller_ops controller_vruntime_overflow;
extern struct controller_ops controller_case_time_sensitive;
extern struct controller_ops controller_noop;

static struct controller_ops *controller_ops_list[] = {
    &controller_even_idle_cpu,
    &controller_extra_balance,
    &controller_freeze,
    &controller_lag_vruntime,
    &controller_long_balance,
    &controller_sync_wakeup,
    &controller_util_avg,
    &controller_vruntime_overflow,
    &controller_case_time_sensitive,
    &controller_noop,
};

struct controller_ops *kstep_controller_get(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(controller_ops_list); i++) {
    if (strcmp(controller_ops_list[i]->name, name) == 0) {
      return controller_ops_list[i];
    }
  }
  panic("Controller %s not found", name);
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

struct controller_ops controller_noop = {
    .name = "noop",
    .init = noop_init,
    .body = noop_body,
};
