#include <linux/delay.h>
#include <linux/kthread.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "utils.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;
static struct task_struct *busy_kthread;
static struct task_struct *busy_kthread_children;
static struct task_struct *pause_task = NULL;

// send data to the busy kthread
static int shared_data = 0;
static atomic_t data_ready = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(my_wq);
static int done = 0;

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
    TRACE_INFO("Receiver: Woke up! Consumed data: %d", shared_data);
    if (kthread_should_stop()) {
      break;
    }

    ksym.try_to_wake_up(busy_kthread_children, TASK_NORMAL, 0 | WF_SYNC);
    atomic_set(&data_ready, 0);
  }
  return 0;
}

static int controller_init(void) {
  busy_kthread = kthread_create(loop, NULL, "busy_kthread");
  set_cpus_allowed_ptr(busy_kthread, cpu_controlled_mask);
  wake_up_process(busy_kthread);

  busy_kthread_children =
      kthread_create(loopBusy, NULL, "busy_kthread_children");
  set_cpus_allowed_ptr(busy_kthread_children, cpu_controlled_mask);
  busy_kthread_children->wake_cpu = 2;

  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  reset_task_stats(busy_kthread);
  reset_task_stats(busy_kthread_children);

  send_sigcode(busy_task, SIGCODE_FORK, 5);

  return 0;
}

static void sleep_all_tasks(int cpu, struct task_struct *target) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == target)
      continue;
    send_sigcode(p, SIGCODE_PAUSE, 0);
    msleep(SIM_INTERVAL_MS);
  }
}

static int controller_step(int iter) {
  struct task_struct *p = find_not_eligible_task(TARGET_TASK, busy_task);

  if (p && done == 0 && task_cpu(p) == task_cpu(busy_kthread)) {
    TRACE_INFO("Found not eligible task %d on cpu %d", p->pid, task_cpu(p));
    sleep_all_tasks(task_cpu(p), p);

    set_cpus_allowed_ptr(busy_kthread, cpumask_of(1));
    set_cpus_allowed_ptr(p, cpumask_of(1));

    shared_data = iter;
    atomic_set(&data_ready, 1);
    wake_up_interruptible(&my_wq);
    pause_task = p;
    done = 1;
  }

  if (done == 2 && pause_task != NULL && pause_task->on_cpu == 1) {
    send_sigcode(pause_task, SIGCODE_PAUSE, 0);
    msleep(SIM_INTERVAL_MS);
    done = 3;
  }

  if (done == 1) {
    done = 2;
  }

  return done == 3;
}

static int controller_exit(void) { return 0; }

struct controller_ops controller_aa3ee4f = {
    .name = "aa3ee4f",
    .init = controller_init,
    .step = controller_step,
    .exit = controller_exit,
};
