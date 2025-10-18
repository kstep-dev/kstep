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

#define SLEEP_MS 100
#define NS_PER_MS 1000000

static struct task_struct *controller_task;
static u64 clock_value = 0;

#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
// On x86_64 with paravirt enabled, `sched_clock` (see `arch/x86/kernel/tsc.c`)
// is a wrapper of `paravirt_sched_clock` which can be changed with
// `paravirt_set_sched_clock` (see `arch/x86/include/asm/paravirt.h`).

static u64 sched_clock(void) {
  if (smp_processor_id() == 0)
    return ksym_kvm_sched_clock_read();
  return clock_value;
}

static void sched_clock_mock(void) {
  ksym_paravirt_set_sched_clock(sched_clock);
  clock_value = ksym_kvm_sched_clock_read();
}

static void sched_clock_restore(void) {
  ksym_paravirt_set_sched_clock(ksym_kvm_sched_clock_read);
}

#elif defined(CONFIG_GENERIC_SCHED_CLOCK)
// On other platforms (e.g., arm64), `sched_clock` is implemented in
// `kernel/time/sched_clock.c`, and we can change the function pointer in
// `struct clock_data` and `struct clock_read_data` to mock the sched clock.

struct clock_data {
  seqcount_latch_t seq;
  struct clock_read_data read_data[2];
  ktime_t wrap_kt;
  unsigned long rate;
  u64 (*actual_read_sched_clock)(void);
};

static struct clock_data cd_backup;

static u64 sched_clock(void) {
  if (smp_processor_id() == 0)
    return cd_backup.actual_read_sched_clock();
  return clock_value;
}

static void sched_clock_mock(void) {
  struct clock_data *cd = ksym_cd;
  memcpy(&cd_backup, cd, sizeof(struct clock_data));
  cd->actual_read_sched_clock = sched_clock;
  for (int i = 0; i < 2; i++) {
    struct clock_read_data *rd = &cd->read_data[i];
    rd->read_sched_clock = sched_clock;
    rd->mult = 1;
    rd->shift = 0;
  }
  clock_value = cd_backup.actual_read_sched_clock();
}

static void sched_clock_restore(void) {
  memcpy(ksym_cd, &cd_backup, sizeof(struct clock_data));
}

#else
#error "Sched clock mocking not supported for this platform"
#endif

static void print_tasks(void) {
  TRACE_DEBUG("\t%3s %c%s %5s %5s %12s %12s %9s", "CPU", ' ', "S", "PID",
              "PPID", "vruntime", "sum-exec", "switches");
  TRACE_DEBUG(
      "\t-------------------------------------------------------------");
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0)
      continue;
    TRACE_DEBUG("\t%3d %c%c %5d %5d %12lld %12lld %9lld", task_cpu(p),
                p->on_cpu ? '>' : ' ', task_state_to_char(p), task_pid_nr(p),
                task_ppid_nr(p), p->se.vruntime, p->se.sum_exec_runtime,
                (long long int)(p->nvcsw + p->nivcsw));
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
  struct cpumask mask = *cpu_active_mask;
  cpumask_clear_cpu(0, &mask);

  int cpu;
  for_each_cpu(cpu, &mask) { ksym_tick_sched_timer_dying(cpu); }
  sched_clock_mock();

  u64 tick_count = 0;
  while (!kthread_should_stop()) {
    tick_count++;

    // Send signal
    msleep(SLEEP_MS / 2);
    struct task_struct *p;
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0) {
        if (tick_count % 5 == 0 && tick_count <= 40) {
          send_sigcode(p, SIGCODE_FORK, 0);
        }
        if (tick_count == 5 || tick_count == 10) {
          send_sigcode(p, SIGCODE_EXIT, 0);
        }
        break;
      }
    }

    // Update clock
    msleep(SLEEP_MS / 2);
    clock_value += SLEEP_MS * NS_PER_MS;
    for_each_cpu(cpu, &mask) {
      struct rq *rq = per_cpu_ptr(ksym_runqueues, cpu);
      TRACE_INFO("Tick %lld: CPU %d nr_running=%d, nr_switches=%lld",
                 tick_count, cpu, rq->nr_running, rq->nr_switches);
    }
    print_tasks();

    // Call tick function
    for_each_cpu(cpu, &mask) {
      smp_call_function_single(cpu, (void *)ksym_sched_tick, NULL, 0);
    }
  }
  sched_clock_restore();
  for_each_cpu(cpu, &mask) {
    smp_call_function_single(cpu, (void *)ksym_tick_setup_sched_timer,
                             (void *)true, 0);
  }
  return 0;
}

static int __init kmod_init(void) {
  init_kernel_symbols();
  controller_task = kthread_create(controller, NULL, "controller");
  set_cpus_allowed_ptr(controller_task, cpumask_of(0));
  wake_up_process(controller_task);
  return 0;
}

static void __exit kmod_exit(void) { kthread_stop(controller_task); }

module_init(kmod_init);
module_exit(kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler control");
