#include "kstep.h"

static struct task_struct *waker_task;
static struct task_struct *wakee_task;
static struct task_struct *other_task;

static atomic_t wakeup_ready = ATOMIC_INIT(0);

static int wakee_main(void *data) {
  while (1)
    __asm__("" : : : "memory");
  return 0;
}

static int waker_main(void *data) {
  while (atomic_read(&wakeup_ready) == 0)
    yield();
  ksym.try_to_wake_up(wakee_task, TASK_NORMAL, WF_SYNC);
  return 0;
}

static void setup(void) {
  other_task = kstep_task_create();

  // Create a waker on cpu 1
  waker_task = kthread_create_on_cpu(waker_main, NULL, 1, "waker");
  wake_up_process(waker_task);

  // Create a wakee. Don't wake up the wakee immediately
  wakee_task = kthread_create(wakee_main, NULL, "wakee");
  struct cpumask mask;
  cpumask_copy(&mask, cpu_active_mask);
  cpumask_clear_cpu(0, &mask);
  set_cpus_allowed_ptr(wakee_task, &mask);
  wakee_task->wake_cpu = 2;

  kstep_reset_task(waker_task);
  kstep_reset_task(wakee_task);
}

static void *is_ineligible(void) {
  if (other_task->on_cpu && !kstep_eligible(&other_task->se))
    return other_task;
  return NULL;
}

static void run(void) {
  kstep_task_pin(other_task, 1, 1);

  kstep_tick_repeat(20);

  // tick until there is a ineligible task
  kstep_tick_until(is_ineligible);

  // wake up the waker to call sync wakeup
  atomic_set(&wakeup_ready, 1);

  // Pause the ineligible task
  kstep_task_pause(other_task);

  // tick to show the impact
  kstep_tick_repeat(10);
}

struct kstep_driver sync_wakeup = {
    .name = "sync_wakeup",
    .setup = setup,
    .run = run,
};
