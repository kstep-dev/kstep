#include "kstep.h"

static struct task_struct *tasks[10];

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++) 
    kstep_task_wakeup(tasks[i]);

  for (int i = 0; i < 15; i++) 
    kstep_tick();
}

struct kstep_driver noop = {
    .name = "noop",
    .setup = setup,
    .run = run,
};
