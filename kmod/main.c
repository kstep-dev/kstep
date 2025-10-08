#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/workqueue.h>

// Linux private headers
#include <kernel/sched/sched.h>

#define KSYM_FUNC_LIST                                                         \
  X(void, tick_sched_timer_dying, (int cpu))                                   \
  X(void, sched_tick, (void))                                                  \
  X(u64, sched_clock, (void))                                                  \
  X(u64, sched_clock_cpu, (int cpu))                                           \
  X(void, paravirt_set_sched_clock, (u64(*func)(void)))                        \
  X(u64, kvm_sched_clock_read, (void))
#define KSYM_VAR_LIST                                                          \
  X(struct rq, runqueues)                                                      \
  X(u64, __sched_clock_offset)
#include "ksym.h"
#include "logging.h"

#define TARGET_TASK_PREFIX "test-proc-"
#define MAX_TARGET_TASKS 10
#define TARGET_CPU 1
#define KTHREAD_SLEEP_MS 500
#define NS_PER_MS 1000000

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler control");

static struct task_struct *target_tasks[MAX_TARGET_TASKS];
static int target_tasks_count = 0;

static u64 mocked_sched_clock_value = 0;
static u64 mocked_sched_clock(void) {
  if (smp_processor_id() == 0)
    return ksym_kvm_sched_clock_read();
  return mocked_sched_clock_value;
}

static void remote_fn(void *data) {
  struct rq *rq = this_cpu_ptr(ksym_runqueues);
  mocked_sched_clock_value += KTHREAD_SLEEP_MS * NS_PER_MS;
  TRACE_INFO("Calling sched_tick on CPU %d: rq->clock=%lld, "
             "sched_clock_cpu=%lld, sched_clock=%lld\n",
             smp_processor_id(), rq->clock,
             ksym_sched_clock_cpu(smp_processor_id()), ksym_sched_clock());
  for (int i = 0; i < target_tasks_count; i++) {
    struct task_struct *task = target_tasks[i];
    TRACE_INFO("pid=%d, comm=%s, vruntime=%llu%s\n", task->pid, task->comm,
               task->se.vruntime, task == current ? " (current)" : "");
  }
  ksym_sched_tick();
}

static int sched_controller(void *data) {
  struct task_struct *task;
  for_each_process(task) {
    if (task_cpu(task) != TARGET_CPU)
      continue;
    if (!strstarts(task->comm, TARGET_TASK_PREFIX))
      continue;
    target_tasks[target_tasks_count++] = task;
  }
  if (target_tasks_count == 0) {
    TRACE_ERROR("No target tasks found\n");
    return -EINVAL;
  }
  while (!kthread_should_stop()) {
    msleep(KTHREAD_SLEEP_MS);
    smp_call_function_single(TARGET_CPU, remote_fn, NULL, 0);
  }
  return 0;
}

static struct task_struct *controller_task;
static int __init kmod_init(void) {
  init_kernel_symbols();

  TRACE_INFO("Freezing CPU %d\n", TARGET_CPU);
  ksym_tick_sched_timer_dying(TARGET_CPU);

  mocked_sched_clock_value = ksym_kvm_sched_clock_read();
  ksym_paravirt_set_sched_clock(mocked_sched_clock);

  // Create kthread for controller
  controller_task = kthread_run(sched_controller, NULL, "sched_controller");
  if (IS_ERR(controller_task)) {
    TRACE_ERROR("Failed to create kthread\n");
    return PTR_ERR(controller_task);
  }

  return 0;
}

static void __exit kmod_exit(void) { kthread_stop(controller_task); }

module_init(kmod_init);
