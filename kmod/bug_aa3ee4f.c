#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched_clock.h>
#include <linux/workqueue.h>

// Linux private headers
#include <kernel/sched/sched.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "sigcode.h"

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

  busy_task->se.vruntime = INIT_TIME_NS;
  busy_kthread->se.vruntime = INIT_TIME_NS;
  busy_kthread_children->se.vruntime = INIT_TIME_NS;

  busy_task->nivcsw = 0;
  busy_kthread->nivcsw = 0;
  busy_kthread_children->nivcsw = 0;

  busy_task->nvcsw = 0;
  busy_kthread->nvcsw = 0;
  busy_kthread_children->nvcsw = 0;

  send_sigcode(busy_task, SIGCODE_FORK, 5);
  msleep(SIM_INTERVAL_MS);

  return 0;
}

static struct task_struct *find_not_eligible_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task)
      continue;
    if (p->on_cpu == 0)
      continue;
    TRACE_DEBUG("pid=%d, eligible=%d, on_cpu=%d", p->pid,
                ksym.entity_eligible(p->se.cfs_rq, &p->se), p->on_cpu);

    if (ksym.entity_eligible(p->se.cfs_rq, &p->se) == 0) {
      return p;
    }
  }
  return NULL;
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
  struct task_struct *p = find_not_eligible_task();

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

  return 0;
}

static int controller_exit(void) { return 0; }

struct controller_ops bug_aa3ee4f_ops = {
    .name = "aa3ee4f",
    .init = controller_init,
    .step = controller_step,
    .exit = controller_exit,
};
