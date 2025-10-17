#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched_clock.h>
#include <linux/workqueue.h>

// Linux private headers
#include <kernel/sched/sched.h>

#define KSYM_FUNC_LIST                                                         \
  X(void, tick_sched_timer_dying, (int cpu))                                   \
  X(void, sched_tick, (void))                                                  \
  X(void, paravirt_set_sched_clock, (u64(*func)(void)))                        \
  X(u64, kvm_sched_clock_read, (void))                                         \
  X(void, tick_setup_sched_timer, (bool hrtimer))
#define KSYM_VAR_LIST                                                          \
  X(struct rq, runqueues)                                                      \
  X(void, cd)
#include "ksym.h"
#include "logging.h"
#include "sigcode.h"

#define TARGET_TASK "test-proc"
#define TARGET_CPU 1

#define SLEEP_MS 500
#define NS_PER_MS 1000000

static struct task_struct *controller_task;
static u64 tick_count = 0;
static u64 clock_value = 0;
static u64 (*original_sched_clock)(void) = NULL;

static u64 sched_clock(void) {
  if (smp_processor_id() == 0)
    return original_sched_clock();
  return clock_value;
}

#ifdef CONFIG_GENERIC_SCHED_CLOCK
// From kernel/time/sched_clock.c
struct clock_data {
  seqcount_latch_t seq;
  struct clock_read_data read_data[2];
  ktime_t wrap_kt;
  unsigned long rate;
  u64 (*actual_read_sched_clock)(void);
};
static struct clock_data original_cd;
static void mock_sched_clock(void) {
  struct clock_data *cd = ksym_cd;
  memcpy(&original_cd, cd, sizeof(struct clock_data));
  original_sched_clock = cd->actual_read_sched_clock;
  cd->actual_read_sched_clock = sched_clock;
  for (int i = 0; i < 2; i++) {
    cd->read_data[i].read_sched_clock = sched_clock;
    cd->read_data[i].mult = 1;
    cd->read_data[i].shift = 0;
  }
}
static void restore_sched_clock(void) {
  memcpy(ksym_cd, &original_cd, sizeof(struct clock_data));
}
#else
static void mock_sched_clock(void) {
  ksym_paravirt_set_sched_clock(sched_clock);
  original_sched_clock = ksym_kvm_sched_clock_read;
}
static void restore_sched_clock(void) {
  ksym_paravirt_set_sched_clock(original_sched_clock);
}
#endif

static void print_tasks(struct rq *rq) {
  TRACE_DEBUG("\t%c%s %5s %5s %12s %12s %9s", ' ', "", "PID", "PPID",
              "vruntime", "sum-exec", "switches");
  TRACE_DEBUG(
      "\t-------------------------------------------------------------");
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) == rq->cpu && strcmp(p->comm, TARGET_TASK) == 0) {
      TRACE_DEBUG("\t%c%c %5d %5d %12lld %12lld %9lld",
                  p == rq->curr ? '>' : ' ', task_state_to_char(p),
                  task_pid_nr(p), task_ppid_nr(p), p->se.vruntime,
                  p->se.sum_exec_runtime,
                  (long long int)(p->nvcsw + p->nivcsw));
    }
  }
}

static void send_sigcode(struct task_struct *p, enum sigcode code, int val) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      .si_int = val,
  };
  send_sig_info(SIGUSR1, &info, p);
  TRACE_INFO("Sent %s (si_int=%d) to pid %d", sigcode_to_str[code], val,
             p->pid);
}

static int controller(void *data) {
  struct rq *rq = per_cpu_ptr(ksym_runqueues, TARGET_CPU);
  u64 clock_value_init = original_sched_clock();
  while (!kthread_should_stop()) {
    tick_count++;

    // Send signal
    msleep(SLEEP_MS / 2);
    if (strcmp(rq->curr->comm, TARGET_TASK) == 0) {
      struct task_struct *p = rq->curr;
      if (tick_count % 5 == 0 && tick_count <= 25)
        send_sigcode(p, SIGCODE_FORK, 0);
      if (tick_count == 5)
        send_sigcode(p, SIGCODE_PAUSE, 0);
    } else {
      TRACE_ERR("The current task is %s", rq->curr->comm);
    }

    // Update clock
    msleep(SLEEP_MS / 2);
    clock_value = clock_value_init + tick_count * SLEEP_MS * NS_PER_MS;
    TRACE_INFO("CPU %d tick %lld: nr_running=%d, nr_switches=%lld", rq->cpu,
               tick_count, rq->nr_running, rq->nr_switches);
    print_tasks(rq);

    // Call tick function
    smp_call_function_single(rq->cpu, (void *)ksym_sched_tick, NULL, 0);
  }
  return 0;
}

static int __init kmod_init(void) {
  init_kernel_symbols();

  // Take over tick timer and mock sched clock
  ksym_tick_sched_timer_dying(TARGET_CPU);
  mock_sched_clock();

  // Create kthread for controller
  controller_task = kthread_run(controller, NULL, "controller");
  if (IS_ERR(controller_task)) {
    TRACE_ERR("Failed to create kthread");
    return PTR_ERR(controller_task);
  }

  TRACE_INFO("Scheduler managed on CPU %d", TARGET_CPU);
  return 0;
}

static void __exit kmod_exit(void) {
  kthread_stop(controller_task);
  restore_sched_clock();
  smp_call_function_single(TARGET_CPU, (void *)ksym_tick_setup_sched_timer,
                           (void *)true, 0);
  TRACE_INFO("Scheduler released on CPU %d", TARGET_CPU);
}

module_init(kmod_init);
module_exit(kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler control");
