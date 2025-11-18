#include "kstep.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;
static struct task_struct *busy_kthread;
static struct task_struct *busy_kthread_children;

static int loopBusy(void *data);
static int loop(void *data);

static void controller_init(void) {
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

  kstep_sleep();

  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  reset_task_stats(busy_kthread);
  reset_task_stats(busy_kthread_children);

  // fork 3 processes on cpu 1
  send_sigcode(busy_task, SIGCODE_FORK, 3);
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) == 0)
      set_cpus_allowed_ptr(p, cpumask_of(1));
  }
}

// send data to the busy kthread
static int shared_data = 0;
static atomic_t data_ready = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(my_wq);

static struct task_struct *find_not_eligible_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task || p->on_cpu == 0)
      continue;

    if (ksym.entity_eligible(p->se.cfs_rq, &p->se) == 0) {
      return p;
    }
  }
  return NULL;
}

static void sleep_all_tasks_except(int cpu, struct task_struct *target) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == target)
      continue;
    send_sigcode(p, SIGCODE_PAUSE, 0);
  }
}

static void controller_body(void) {
  for (int i = 0; i < 20; i++) {
    call_tick_once(true);
  }

  struct task_struct *not_eligible_task = NULL;
  // tick until there is a not eligible task on the same cpu as the busy kthread
  while (1) {
    not_eligible_task = find_not_eligible_task();
    if (not_eligible_task && task_cpu(not_eligible_task) == task_cpu(busy_kthread)) {
      break;
    }
    call_tick_once(true);
  }

  // sleep all colocated tasks 
  TRACE_INFO("Found not eligible task %d on cpu %d", not_eligible_task->pid, task_cpu(not_eligible_task));
  sleep_all_tasks_except(task_cpu(not_eligible_task), not_eligible_task);
  
  call_tick_once(true);
  
  // wake up the kthread to call sync wakeup
  shared_data = 0;
  atomic_set(&data_ready, 1);
  wake_up_interruptible(&my_wq);

  call_tick_once(true);

  // tick until the not eligible task is running on the cpu and pause it
  while (1) {
    if (not_eligible_task->on_cpu == 1) {
      send_sigcode(not_eligible_task, SIGCODE_PAUSE, 0);
      break;
    }
    call_tick_once(true);
  }

  // tick to show the impact
  for (int i = 0; i < 20; i++) {
    call_tick_once(true);
  }
}

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

struct controller_ops controller_aa3ee4f = {
    .name = "aa3ee4f",
    .init = controller_init,
    .body = controller_body,
};
