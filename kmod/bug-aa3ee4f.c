#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/reboot.h> // For kernel_power_off()
#include <linux/sched_clock.h>
#include <linux/workqueue.h>

// Linux private headers
#include <kernel/sched/sched.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "sigcode.h"
#include "utils.h"

#define SIM_INTERVAL_MS 100
#define TICK_INTERVAL_NS (1000ULL * 1000ULL)               // 1 ms
#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s
#define TARGET_TASK "test-proc"

static struct task_struct *controller_task;
static struct task_struct *busy_task;
static struct task_struct *busy_kthread;
static struct task_struct *busy_kthread_children;

static void print_tasks(void) {
  int cpu;
  for_each_controlled_cpu(cpu) {
    struct rq *rq = per_cpu_ptr(ksym.runqueues, cpu);
    TRACE_INFO("- CPU %d running=%d, switches=%3lld, clock=%lld, avg_load=%lld",
               cpu, rq->nr_running, rq->nr_switches, rq->clock,
               rq->cfs.avg_load);
  }

  TRACE_DEBUG("\t%3s %c%s %5s %5s %12s %12s %9s", "CPU", ' ', "S", "PID",
              "PPID", "vruntime", "sum-exec", "switches");
  TRACE_DEBUG(
      "\t-------------------------------------------------------------");
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 && p != busy_kthread &&
        p != busy_kthread_children)
      continue;
    // TRACE_DEBUG("p->pid=%d, p->ppid=%d", task_pid_nr(busy_task),
    // task_ppid_nr(busy_task));
    TRACE_DEBUG("\t%3d %c%c %5d %5d %12lld %12lld %4lu+%-4lu", task_cpu(p),
                p->on_cpu ? '>' : ' ', task_state_to_char(p),
                task_pid_nr(p) - task_pid_nr(busy_kthread), task_ppid_nr(p),
                p->se.vruntime, p->se.sum_exec_runtime, p->nvcsw, p->nivcsw);
  }
}

static struct task_struct *poll_target_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    TRACE_DEBUG("pid=%d, comm=%s, state=%x, on_cpu=%d", p->pid, p->comm,
                p->__state, p->on_cpu);
  }
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0)
        return p;
    }
    msleep(SIM_INTERVAL_MS);
    TRACE_INFO("Waiting for process %s to be created", TARGET_TASK);
  }
}

static int loopBusy(void *data);
static int loop(void *data);

static void controller_init(void) {
  int cpu;

  busy_kthread = kthread_create(loop, NULL, "busy_kthread");
  set_cpus_allowed_ptr(busy_kthread, cpumask_of(1));
  // set_cpus_allowed_ptr(busy_kthread, & cpu_controlled_mask);
  wake_up_process(busy_kthread);

  busy_kthread_children =
      kthread_create(loopBusy, NULL, "busy_kthread_children");
  set_cpus_allowed_ptr(busy_kthread_children, cpu_controlled_mask);
  busy_kthread_children->wake_cpu = 2;

  // set_cpus_allowed_ptr(busy_kthread, &mask);
  msleep(SIM_INTERVAL_MS);

  busy_task = poll_target_task();
  reset_task_stats(busy_task);
  reset_task_stats(busy_kthread);
  reset_task_stats(busy_kthread_children);

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
  }
}

static void call_tick_once(void) {
  print_tasks();
  sched_clock_inc(TICK_INTERVAL_NS);

  // Call tick function
  int cpu;
  for_each_controlled_cpu(cpu) {
    smp_call_function_single(cpu, (void *)ksym.sched_tick, NULL, 0);
    msleep(SIM_INTERVAL_MS);
  }
}
static struct task_struct *pause_task = NULL;
static void controller_body(void) {
  for (int i = 0; i < 20; i++) {
    call_tick_once();
  }
  while (1) {
    struct task_struct *p = find_not_eligible_task();
    if (p && task_cpu(p) == task_cpu(busy_kthread)) {
      TRACE_INFO("Found not eligible task %d on cpu %d", p->pid, task_cpu(p));
      sleep_all_tasks(task_cpu(p), p);

      shared_data = 0;
      atomic_set(&data_ready, 1);
      wake_up_interruptible(&my_wq);
      call_tick_once();
      pause_task = p;
      break;
    }
    call_tick_once();
  }

  while (1) {
    if (pause_task->on_cpu == 1) {
      send_sigcode(pause_task, SIGCODE_PAUSE, 0);
      break;
    }
    call_tick_once();
  }

  for (int i = 0; i < 20; i++) {
    call_tick_once();
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
