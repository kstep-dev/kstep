#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched_clock.h>
#include <linux/workqueue.h>
#include <linux/reboot.h> // For kernel_power_off()
#include <linux/freezer.h>

// Linux private headers
#include <kernel/sched/sched.h>

#define KSYM_FUNC_LIST                                                         \
  X(void, tick_sched_timer_dying, (int cpu))                                   \
  X(void, sched_tick, (void))                                                  \
  X(void, paravirt_set_sched_clock, (u64(*func)(void)))                        \
  X(u64, kvm_sched_clock_read, (void))                                         \
  X(void, tick_setup_sched_timer, (bool hrtimer))                              \
  X(u64, sched_clock, (void))                                                  \
  X(void, update_rq_clock, (struct rq * rq))                                   \
  X(int, entity_eligible, (struct cfs_rq *cfs_rq, struct sched_entity *se))    \
  X(void, signal_wake_up_state, (struct task_struct * t, int state))           \
  X(void, try_to_wake_up, (struct task_struct *p, unsigned int state, int wake_flags)) \
  X(void, sched_yield, (void))                                                 \
  X(void, freeze_task, (struct task_struct *p))
#define KSYM_VAR_LIST                                                          \
  X(struct rq, runqueues)                                                      \
  X(void, cd)                                                                  \
  X(u64, __sched_clock_offset)                                                 \
  X(unsigned int, sysctl_sched_migration_cost)                                 \
  X(bool, pm_freezing)
#include "ksym.h"
#include "logging.h"
#include "sigcode.h"

#define SIM_INTERVAL_MS 100
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

static struct task_struct *controller_task;
static struct task_struct *busy_task;

static void print_tasks(void) {
  int cpu;
  for_each_controlled_cpu(cpu) {
    struct rq *rq = per_cpu_ptr(ksym_runqueues, cpu);
    TRACE_INFO("- CPU %d running=%d, switches=%3lld, clock=%lld, avg_load=%lld",
               cpu, rq->nr_running - (rq->cfs.h_nr_queued - rq->cfs.h_nr_runnable), rq->nr_switches, rq->clock,
               rq->cfs.avg_load);
  }

  TRACE_DEBUG("\t%3s %c%s %5s %5s %12s %12s %9s", "CPU", ' ', "S", "PID",
              "PPID", "vruntime", "sum-exec", "switches");
  TRACE_DEBUG(
      "\t-------------------------------------------------------------");
  struct task_struct *p;
  for_each_process(p) {
    // if (strcmp(p->comm, TARGET_TASK) != 0 && p != busy_kthread && p != busy_kthread_children)
    //   continue;
    if (task_cpu(p) == 0)
      continue;
    // TRACE_DEBUG("p->pid=%d, p->ppid=%d", task_pid_nr(busy_task), task_ppid_nr(busy_task));
    TRACE_DEBUG("\t%3d %c%c %5d %5d %12lld %12lld %4lu+%-4lu", task_cpu(p),
                p->on_cpu ? '>' : ' ', task_state_to_char(p), task_pid_nr(p),
                task_ppid_nr(p), p->se.vruntime, p->se.sum_exec_runtime,
                p->nvcsw, p->nivcsw);
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
  for_each_process(p) {
    TRACE_DEBUG("pid=%d, comm=%s, state=%x, on_cpu=%d", p->pid, p->comm, p->__state, p->on_cpu);
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

static void controller_init(void) {
  int cpu;
  for_each_controlled_cpu(cpu) { ksym_tick_sched_timer_dying(cpu); }
  sched_clock_init();


  // set_cpus_allowed_ptr(busy_kthread, &mask);
  msleep(SIM_INTERVAL_MS);
  
  busy_task = poll_target_task();

  busy_task->se.vruntime = INIT_TIME_NS;
  busy_task->nivcsw = 0;
  busy_task->nvcsw = 0;

  for_each_controlled_cpu(cpu) {
    struct rq *rq = per_cpu_ptr(ksym_runqueues, cpu);
    struct sched_domain *sd;

    ksym_update_rq_clock(rq);
    rq->avg_idle = 2 * *ksym_sysctl_sched_migration_cost;
    rq->max_idle_balance_cost = *ksym_sysctl_sched_migration_cost;
    rq->nr_switches = 0;

    rq->cfs.min_vruntime = INIT_TIME_NS;
    
    for (sd = rcu_dereference_check_sched_domain(rq->sd); \
			sd; sd = sd->parent) {
      sd->last_balance = jiffies;
      sd->balance_interval = sd->min_interval;
      sd->nr_balance_failed = 0;
    }
  }


  send_sigcode(busy_task, SIGCODE_FORK, 5);
  msleep(SIM_INTERVAL_MS);
  
}

static void controller_exit(void) {
  int cpu;
  sched_clock_exit();
  for_each_controlled_cpu(cpu) {
    smp_call_function_single(cpu, (void *)ksym_tick_setup_sched_timer,
                             (void *)true, 0);
  }

  kernel_power_off();
}

// send data to the busy kthread
static int shared_data = 0;
static atomic_t data_ready;
static DECLARE_WAIT_QUEUE_HEAD(my_wq);
int done = 0;


static struct task_struct *find_not_eligible_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task)
      continue;
    if (p->on_cpu == 0)
      continue;
    TRACE_DEBUG("pid=%d, eligible=%d, on_cpu=%d", p->pid, ksym_entity_eligible(p->se.cfs_rq, &p->se), p->on_cpu);
    
    if (ksym_entity_eligible(p->se.cfs_rq, &p->se) == 0) {
      return p;
    }
  }
  return NULL;
}

struct task_struct * pause_task = NULL;
static void controller_step(int iter) {
  // Update clock
  print_tasks();
  clock_value += TICK_INTERVAL_NS;

  struct task_struct *p = find_not_eligible_task();
  
  if (p && done == 0) {
    TRACE_INFO("dequeue ineligible task %d", p->pid);
    pause_task = p;
    send_sigcode(pause_task, SIGCODE_PAUSE, 1000);

    msleep(SIM_INTERVAL_MS);
    done = 1;
  }

  if (done == 2 && pause_task != NULL) {
    TRACE_INFO("freeze ineligible task %d", pause_task->pid);
    static_branch_inc(&freezer_active);
    * ksym_pm_freezing = true;
    pause_task->__state |= TASK_FREEZABLE;
    pause_task->__state |= TASK_INTERRUPTIBLE;
    ksym_freeze_task(pause_task);
    
    * ksym_pm_freezing = false;
    static_branch_dec(&freezer_active);
    msleep(SIM_INTERVAL_MS);
    done = 3;
  }
  if (done == 5 && pause_task != NULL) {
    TRACE_INFO("wake up ineligible task %d", pause_task->pid);
    ksym_try_to_wake_up(pause_task, TASK_NORMAL, 0);
    msleep(SIM_INTERVAL_MS);
    done = 6;
  }


  // Call tick function
  int cpu;
  for_each_controlled_cpu(cpu) {
    smp_call_function_single(cpu, (void *)ksym_sched_tick, NULL, 0);
    msleep(SIM_INTERVAL_MS);
  }
  if (done == 1) {
    done = 2;
  }
  if (done == 3 || done == 4) {
    done++;
  }
}

static int controller(void *data) {
  controller_init();
  int iter = 0;
  while (!kthread_should_stop()) {
    controller_step(iter++);
    if ((iter % 1000 == 0)) {
      break;
    }
  }
  controller_exit();
  return 0;
}

static int __init kmod_init(void) {
  init_kernel_symbols();
  init_controlled_mask();

  atomic_set(&data_ready, 0);

  controller_task = kthread_create(controller, NULL, "controller");
  set_cpus_allowed_ptr(controller_task, cpumask_of(0));
  wake_up_process(controller_task);
  return 0;
}

static void __exit kmod_exit(void) { 
  kthread_stop(controller_task); 
}

module_init(kmod_init);
module_exit(kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler control");
