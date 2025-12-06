#include "kstep.h"

static struct task_struct *busy_kthread;
static struct task_struct *busy_kthread_children;
static struct task_struct *ineligible_task;
static struct task_struct *tasks[3];

// send data to the busy kthread
static atomic_t data_ready = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(my_wq);

static int loopBusy(void *data) {
  TRACE_INFO("Sync wakeup's children started on CPU %d", smp_processor_id());
  while (!kthread_should_stop()) {
    __asm__("" : : : "memory");
  }
  return 0;
}

static int loop(void *data) {
  TRACE_INFO("Busy kthread started on CPU %d", smp_processor_id());
  while (!kthread_should_stop()) {
    wait_event_interruptible(my_wq, atomic_read(&data_ready) != 0 ||
                                        kthread_should_stop());
    TRACE_INFO("Receiver: Woke up! Consumed data");
    if (kthread_should_stop()) {
      break;
    }

    ksym.try_to_wake_up(busy_kthread_children, TASK_NORMAL, 0 | WF_SYNC);
    atomic_set(&data_ready, 0);
  }
  return 0;
}

static void init(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();

  // create a busy kthread on cpu 1
  busy_kthread = kthread_create(loop, NULL, "busy_kthread");
  set_cpus_allowed_ptr(busy_kthread, cpumask_of(1));
  wake_up_process(busy_kthread);

  // create a busy kthread children on all controlled cpus
  // don't wake up the children immediately
  busy_kthread_children =
      kthread_create(loopBusy, NULL, "busy_kthread_children");
  struct cpumask mask;
  cpumask_copy(&mask, cpu_active_mask);
  cpumask_clear_cpu(0, &mask);
  set_cpus_allowed_ptr(busy_kthread_children, &mask);
  busy_kthread_children->wake_cpu = 2;

  reset_task_stats(busy_kthread);
  reset_task_stats(busy_kthread_children);
}

static void *is_ineligible(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    if (tasks[i]->on_cpu && task_cpu(tasks[i]) == task_cpu(busy_kthread) &&
        !kstep_eligible(&tasks[i]->se))
      return tasks[i];
  return NULL;
}

static void *is_running_again(void) {
  return ineligible_task->on_cpu == 1 ? ineligible_task : NULL;
}

static void body(void) {
  for (int i = 1; i < ARRAY_SIZE(tasks); i++)
    kstep_task_pin(tasks[i], 1, 1);

  kstep_tick_repeat(20);

  // tick until there is a ineligible task on the same cpu as the busy kthread
  ineligible_task = kstep_tick_until(is_ineligible);

  // sleep all colocated tasks
  TRACE_INFO("Found ineligible task %d on cpu %d", ineligible_task->pid,
             task_cpu(ineligible_task));
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    if (tasks[i] != ineligible_task)
      kstep_task_pause(tasks[i]);

  kstep_tick();

  // wake up the kthread to call sync wakeup
  atomic_set(&data_ready, 1);
  wake_up_interruptible(&my_wq);

  kstep_tick();

  // tick until the not eligible task is running on the cpu and pause it
  kstep_tick_until(is_running_again);
  kstep_task_pause(ineligible_task);

  // tick to show the impact
  kstep_tick_repeat(8);
}

struct kstep_driver sync_wakeup = {
    .name = "sync_wakeup",
    .init = init,
    .body = body,
};
