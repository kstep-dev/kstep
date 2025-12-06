#include "kstep.h"

static struct task_struct *waker_task;
static struct task_struct *wakee_task;
static struct task_struct *tasks[3];

// To notify the waker
static DECLARE_COMPLETION(wakeup_ready);

static int wakee_main(void *data) {
  TRACE_INFO("Wakee started on CPU %d", smp_processor_id());
  while (!kthread_should_stop())
    __asm__("" : : : "memory");
  return 0;
}

static int waker_main(void *data) {
  TRACE_INFO("Waker started on CPU %d", smp_processor_id());
  wait_for_completion(&wakeup_ready);
  ksym.try_to_wake_up(wakee_task, TASK_NORMAL, WF_SYNC);
  return 0;
}

static void init(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();

  // Create a waker on cpu 1
  waker_task = kthread_create(waker_main, NULL, "waker");
  set_cpus_allowed_ptr(waker_task, cpumask_of(1));
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
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    if (tasks[i]->on_cpu && !kstep_eligible(&tasks[i]->se))
      return tasks[i];
  return NULL;
}

static void body(void) {
  for (int i = 1; i < ARRAY_SIZE(tasks); i++)
    kstep_task_pin(tasks[i], 1, 1);

  kstep_tick_repeat(20);

  // tick until there is a ineligible task on the same cpu as the busy kthread
  struct task_struct *ineligible_task = kstep_tick_until(is_ineligible);
  TRACE_INFO("Found ineligible task %d on cpu %d", ineligible_task->pid,
             task_cpu(ineligible_task));

  // sleep all other tasks
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    if (tasks[i] != ineligible_task)
      kstep_task_pause(tasks[i]);

  // wake up the waker to call sync wakeup
  complete(&wakeup_ready);

  // Pause the ineligible task
  kstep_task_pause(ineligible_task);

  // tick to show the impact
  kstep_tick_repeat(10);
}

struct kstep_driver sync_wakeup = {
    .name = "sync_wakeup",
    .init = init,
    .body = body,
};
