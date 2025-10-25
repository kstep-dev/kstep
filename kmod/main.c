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
  X(void, tick_setup_sched_timer, (bool hrtimer))                              \
  X(u64, sched_clock, (void))                                                  \
  X(int, workqueue_offline_cpu, (int cpu))
#define KSYM_VAR_LIST                                                          \
  X(struct rq, runqueues)                                                      \
  X(void, cd)                                                                  \
  X(u64, __sched_clock_offset)                                                 \
  X(unsigned int, sysctl_sched_migration_cost)
#include "ksym.h"
#include "logging.h"
#include "sigcode.h"

#define SIM_INTERVAL_MS 10
#define TICK_INTERVAL_NS (1000ULL * 1000ULL)               // 1 ms
#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s
#define TARGET_TASK "test-proc"

static struct cpumask cpu_controlled_mask;
#define for_each_controlled_cpu(cpu) for_each_cpu(cpu, &cpu_controlled_mask)
static void init_controlled_mask(void) {
  cpumask_copy(&cpu_controlled_mask, cpu_active_mask);
  cpumask_clear_cpu(0, &cpu_controlled_mask);
}

static u64 clock_value = INIT_TIME_NS;
static u64 sched_clock(void) { return clock_value; }

#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
// On x86_64 with paravirt enabled, `sched_clock` (see `arch/x86/kernel/tsc.c`)
// is a wrapper of `paravirt_sched_clock` which can be changed with
// `paravirt_set_sched_clock` (see `arch/x86/include/asm/paravirt.h`).

static void sched_clock_init(void) {
  *ksym___sched_clock_offset = 0;
  ksym_paravirt_set_sched_clock(sched_clock);
}

static void sched_clock_exit(void) {
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

static void sched_clock_init(void) {
  struct clock_data *cd = ksym_cd;
  memcpy(&cd_backup, cd, sizeof(struct clock_data));
  cd->actual_read_sched_clock = sched_clock;
  for (int i = 0; i < 2; i++) {
    struct clock_read_data *rd = &cd->read_data[i];
    rd->read_sched_clock = sched_clock;
    rd->mult = 1;
    rd->shift = 0;
    rd->epoch_ns = 0;
    rd->epoch_cyc = 0;
  }
}

static void sched_clock_exit(void) {
  memcpy(ksym_cd, &cd_backup, sizeof(struct clock_data));
}

#else
#error "Sched clock mocking not supported for this platform"
#endif

static void print_tasks(void) {
  int cpu;
  for_each_controlled_cpu(cpu) {
    struct rq *rq = per_cpu_ptr(ksym_runqueues, cpu);
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
    if (task_cpu(p) == 0)
      continue;
    TRACE_DEBUG("\t%3d %c%c %5d %5d %12lld %12lld %4lu+%-4lu %s", task_cpu(p),
                p->on_cpu ? '>' : ' ', task_state_to_char(p), task_pid_nr(p),
                task_ppid_nr(p), p->se.vruntime, p->se.sum_exec_runtime,
                p->nvcsw, p->nivcsw, p->comm);
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

static struct task_struct *poll_target_task(void) {
  struct task_struct *p;
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0)
        return p;
    }
    msleep(SIM_INTERVAL_MS);
    TRACE_INFO("Waiting for process %s to be created", TARGET_TASK);
  }
}

static void controller_init(void) {
  // Disable timer ticks and workqueue on all controlled CPUs
  {
    int cpu;
    for_each_controlled_cpu(cpu) {
      ksym_tick_sched_timer_dying(cpu);
      smp_call_function_single(cpu, (void *)ksym_workqueue_offline_cpu,
                               (void *)(intptr_t)cpu, 1);
    }
  }

  // Move non-essential kernel threads to CPU 0
  {
    struct task_struct *p;
    for_each_process(p) {
      if (task_cpu(p) == 0)
        continue;
      if (strncmp(p->comm, "cpuhp/", 6) == 0 ||
          strncmp(p->comm, "migration/", 10) == 0 ||
          strncmp(p->comm, "ksoftirqd/", 10) == 0)
        continue;
      set_cpus_allowed_ptr(p, cpumask_of(0));
      wake_up_process(p);
    }
  }

  sched_clock_init();

  struct task_struct *p = poll_target_task();
  send_sigcode(p, SIGCODE_FORK, 4);
  p->se.vruntime = INIT_TIME_NS;
  p->nivcsw = 0;
  p->nvcsw = 0;

  int cpu;
  for_each_controlled_cpu(cpu) {
    struct rq *rq = per_cpu_ptr(ksym_runqueues, cpu);
    rq->avg_idle = 2 * *ksym_sysctl_sched_migration_cost;
    rq->max_idle_balance_cost = *ksym_sysctl_sched_migration_cost;
    rq->nr_switches = 0;

    rq->cfs.min_vruntime = INIT_TIME_NS;
  }
}

static void controller_exit(void) {
  int cpu;
  sched_clock_exit();
  for_each_controlled_cpu(cpu) {
    smp_call_function_single(cpu, (void *)ksym_tick_setup_sched_timer,
                             (void *)true, 0);
  }
}

static void controller_step(int iter) {
  // Update clock
  print_tasks();
  clock_value += TICK_INTERVAL_NS;
  int cpu;
  for_each_controlled_cpu(cpu) {
    struct rq *rq = per_cpu_ptr(ksym_runqueues, cpu);
    // force balance at every tick
    rq->next_balance = 0;
    for (struct sched_domain *sd = rq->sd; sd; sd = sd->parent) {
      sd->last_balance = 0;
    }
  }

  // Call tick function
  for_each_controlled_cpu(cpu) {
    smp_call_function_single(cpu, (void *)ksym_sched_tick, NULL, 1);
  }
  msleep(SIM_INTERVAL_MS);
}

static int controller(void *data) {
  controller_init();
  int iter = 0;
  while (!kthread_should_stop()) {
    controller_step(iter++);
  }
  controller_exit();
  return 0;
}

static struct task_struct *controller_task;

static int __init kmod_init(void) {
  init_kernel_symbols();
  init_controlled_mask();
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
