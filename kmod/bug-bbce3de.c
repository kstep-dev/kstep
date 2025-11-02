#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched_clock.h>
#include <linux/workqueue.h>
#include <linux/reboot.h> // For kernel_power_off()

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
  X(void, signal_wake_up_state, (struct task_struct * t, int state))          \
  X(void, try_to_wake_up, (struct task_struct *p, unsigned int state, int wake_flags)) \
  X(void, sched_yield, (void))
#define KSYM_VAR_LIST                                                          \
  X(struct rq, runqueues)                                                      \
  X(void, cd)                                                                  \
  X(u64, __sched_clock_offset)                                                 \
  X(unsigned int, sysctl_sched_migration_cost)                                 
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

static struct task_struct *controller_task = NULL;
static struct task_struct *busy_task = NULL;
static struct task_struct *cgroup_task = NULL;

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
    if (strcmp(p->comm, TARGET_TASK) != 0)
      continue;
    // TRACE_DEBUG("p->pid=%d, p->ppid=%d", task_pid_nr(busy_task), task_ppid_nr(busy_task));
    TRACE_DEBUG("\t%3d %c%c %5d %5d %12lld %12lld %4lu+%-4lu", task_cpu(p),
                p->on_cpu ? '>' : ' ', task_state_to_char(p), task_pid_nr(p) - task_pid_nr(busy_task),
                0, p->se.vruntime, p->se.sum_exec_runtime,
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
  msleep(SIM_INTERVAL_MS);
}

static void poll_target_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    TRACE_DEBUG("pid=%d, comm=%s, state=%x, on_cpu=%d", p->pid, p->comm, p->__state, p->on_cpu);
  }
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0)
        busy_task = p;
      if (strcmp(p->comm, "cgroup-proc") == 0)
        cgroup_task = p;
      if (cgroup_task != NULL && busy_task != NULL)
        return;
    }
    msleep(SIM_INTERVAL_MS);
    TRACE_INFO("Waiting for process %s to be created", TARGET_TASK);
  }
}

static int task_to_cgroup_id[128][2];

static void record_task_groups(int val, int level_id, int id_in_level) {
  struct task_struct *p;
  int count = 0;
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0 && task_to_cgroup_id[p->pid - busy_task->pid][0] == -1) {
        TRACE_INFO("Recording task_to_cgroup_id: %d level_id: %d id_in_level: %d", p->pid, level_id, id_in_level);
        task_to_cgroup_id[p->pid - busy_task->pid][0] = level_id;
        task_to_cgroup_id[p->pid - busy_task->pid][1] = id_in_level;
        if (++count == val) {
          return;
        }
      }
    }
    msleep(SIM_INTERVAL_MS);
    TRACE_INFO("Waiting for recording task_to_cgroup_id: %d level_id: %d id_in_level: %d", val, level_id, id_in_level);
  }
}


static void controller_init(void) {
  int cpu;
  for (int i = 0; i < 128; i++) {
    task_to_cgroup_id[i][0] = -1;
    task_to_cgroup_id[i][1] = -1;
  }
  task_to_cgroup_id[busy_task->pid - busy_task->pid][0] = 0;
  task_to_cgroup_id[busy_task->pid - busy_task->pid][1] = 0;

  for_each_controlled_cpu(cpu) { ksym_tick_sched_timer_dying(cpu); }
  sched_clock_init();
  
  poll_target_task();
  TRACE_INFO("Found busy task: %d, cgroup task: %d", busy_task->pid, cgroup_task->pid);

  busy_task->se.exec_start		= 0;
	busy_task->se.sum_exec_runtime		= 0;
	busy_task->se.prev_sum_exec_runtime	= 0;
	busy_task->se.nr_migrations		= 0;
	busy_task->se.vruntime			= INIT_TIME_NS;
	busy_task->se.vlag			= 0;

  memset(&busy_task->se.avg, 0, sizeof(struct sched_avg));
  busy_task->se.avg.last_update_time = INIT_TIME_NS;
  busy_task->se.avg.load_avg = scale_load_down(busy_task->se.load.weight);

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
    rq->cfs.avg_vruntime = 0;
    rq->cfs.avg_load = 0;

    memset(&rq->cfs.avg, 0, sizeof(struct sched_avg));
    rq->cfs.avg.last_update_time = INIT_TIME_NS;
    
    for (sd = rcu_dereference_check_sched_domain(rq->sd); \
			sd; sd = sd->parent) {
      sd->last_balance = jiffies;
      sd->balance_interval = sd->min_interval;
      sd->nr_balance_failed = 0;
    }
  }

  /*
    create a cgroup tree
    root -> l1_0 -> l2_0 -> l3_0
                 -> l2_1 -> l3_1
  */

  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 1 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 1 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 2 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 2 << 16 | 0x0);

  send_sigcode(busy_task, SIGCODE_CLONE3_L3_0, 1);
  record_task_groups(1, 3, 0);
  send_sigcode(busy_task, SIGCODE_CLONE3_L3_1, 5);
  record_task_groups(5, 3, 1);
}

static void controller_exit(void) {
  int cpu;
  sched_clock_exit();
  for_each_controlled_cpu(cpu) {
    smp_call_function_single(cpu, (void *)ksym_tick_setup_sched_timer,
                             (void *)true, 0);
  }

  // kernel_power_off();
}

static struct task_struct * ineligible_task = NULL;
static struct sched_entity * ineligible_tg_se = NULL;
static int cpu_of_ineligible_task = -1;

static void find_not_eligible_tg(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task || p->on_cpu == 0)
      continue;
    struct sched_entity *se = &p->se;
    // TRACE_DEBUG("pid=%d, eligible=%d", p->pid, ksym_entity_eligible(se->parent->cfs_rq, se->parent));
    if (ineligible_task == NULL &&
        ksym_entity_eligible(se->parent->cfs_rq, se->parent) == 0 && 
        ksym_entity_eligible(se->cfs_rq, se) == 1) {
      ineligible_task = p;
      ineligible_tg_se = se->parent;
      cpu_of_ineligible_task = task_cpu(p);
    }
  }
}

static void sleep_all_tasks_in_ineligible_tg(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task)
      continue;
    if (p->se.parent != ineligible_tg_se)
      continue;
    send_sigcode(p, SIGCODE_PAUSE, 0);
  }
}

static struct task_struct * get_task_by_cpu(int cpu) {
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) == cpu && p->on_cpu == 1)
      return p;
  }
  return NULL;
}

static struct task_struct * get_curr_task(int cpu) {
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) == cpu && p->on_cpu == 1)
      return p;
  }
  return NULL;
}

static void call_tick_once(void) {
  print_tasks();
  clock_value += TICK_INTERVAL_NS;

  // Call tick function
  int cpu;
  for_each_controlled_cpu(cpu) {
    smp_call_function_single(cpu, (void *)ksym_sched_tick, NULL, 0);
    msleep(SIM_INTERVAL_MS);
  }

}

static void controller_step(int iter) {
  // Update clock
  for (int i = 0; i < 20; i++) {
    call_tick_once();
  }

  while (1) {
      find_not_eligible_tg();
      if (ineligible_tg_se != NULL) {
        TRACE_INFO("Found not eligible task group");
        sleep_all_tasks_in_ineligible_tg();
        break;
      }
      call_tick_once();
  }

  call_tick_once();

  while (1) {
    struct task_struct *p = get_task_by_cpu(cpu_of_ineligible_task);
    if (p != NULL) {
      TRACE_INFO("Cloning task to l2_1 from %d", p->pid);
      send_sigcode(p, SIGCODE_CLONE3_L2_1, 1);
      record_task_groups(1, 2, 1);
      // cpu_of_ineligible_task = -1;
      break;
    }
    call_tick_once();
  }

  while (1) {
    if (ineligible_tg_se->sched_delayed == 0) {
      TRACE_INFO("Ineligible task group is cleared at cpu: %d", cpu_of_ineligible_task);
      break;
    }
    call_tick_once();
  } 

  for (int i = 0; i < 60; i++) {
    call_tick_once();
  }
}

static int controller(void *data) {
  controller_init();
  int iter = 0;
  while (!kthread_should_stop()) {
    controller_step(iter++);
    break;
  }
  controller_exit();
  return 0;
}

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
