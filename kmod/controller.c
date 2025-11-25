#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/reboot.h>

#include "kstep.h"

extern struct controller_ops controller_even_idle_cpu;
extern struct controller_ops controller_extra_balance;
extern struct controller_ops controller_freeze;
extern struct controller_ops controller_lag_vruntime;
extern struct controller_ops controller_long_balance;
extern struct controller_ops controller_sync_wakeup;
extern struct controller_ops controller_util_avg;
extern struct controller_ops controller_vruntime_overflow;
extern struct controller_ops controller_case_time_sensitive;
extern struct controller_ops controller_noop;

static struct controller_ops *controller_ops_list[] = {
    &controller_even_idle_cpu,
    &controller_extra_balance,
    &controller_freeze,
    &controller_lag_vruntime,
    &controller_long_balance,
    &controller_sync_wakeup,
    &controller_util_avg,
    &controller_vruntime_overflow,
    &controller_case_time_sensitive,
    &controller_noop,
};

struct controller_ops *kstep_controller_get(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(controller_ops_list); i++) {
    if (strcmp(controller_ops_list[i]->name, name) == 0) {
      return controller_ops_list[i];
    }
  }
  panic("Controller %s not found", name);
}

static void disable_workqueue(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.workqueue_offline_cpu,
                             (void *)(uintptr_t)cpu, 1);
    TRACE_INFO("Disabled workqueue on CPU %d", cpu);
  }
}

// Prealloc kworkers for workqueue to avoid non-deterministic behavior.
static DECLARE_COMPLETION(dummy_start);
static DECLARE_COMPLETION(dummy_done);
static void dummy_work_fn(struct work_struct *work) {
  complete(&dummy_start);
  wait_for_completion(&dummy_done);
}

static void prealloc_kworker(struct workqueue_struct *wq, int num_kworkers) {
  reinit_completion(&dummy_start);
  reinit_completion(&dummy_done);
  int num_cpus = num_online_cpus();
  int num_works = num_kworkers * num_cpus;
  struct work_struct *dummy_works =
      kcalloc(num_works, sizeof(struct work_struct), GFP_KERNEL);
  for (int i = 0; i < num_works; i++) {
    INIT_WORK(&dummy_works[i], dummy_work_fn);
    int cpu = i / num_kworkers;
    queue_work_on(cpu, wq, &dummy_works[i]);
  }
  for (int i = 0; i < num_works; i++) {
    wait_for_completion(&dummy_start);
  }
  complete_all(&dummy_done);
  for (int i = 0; i < num_works; i++) {
    flush_work(&dummy_works[i]);
  }
  kfree(dummy_works);
}

static void prealloc_kworkers(void) {
  prealloc_kworker(system_wq, 2);
  prealloc_kworker(system_highpri_wq, 2);
}

static void move_kthreads(void) {
  struct task_struct *p;
  for_each_process(p) {
    // Skip if 0 is the only allowed cpu
    if (cpumask_test_cpu(0, &p->cpus_mask) &&
        cpumask_weight(&p->cpus_mask) == 1) {
      continue;
    }
    // skip non-kthreads
    if (!(p->flags & PF_KTHREAD)) {
      continue;
    }

    if (is_sys_kthread(p)) {
      reset_task_stats(p);
      continue;
    }
    set_cpus_allowed_ptr(p, cpumask_of(0));
    wake_up_process(p);
    kstep_sleep(); // sometimes kworker/1:2H can be started very late
                   // and miss the move_kthreads
  }
  TRACE_INFO("Moved kthreads to CPU 0");
}

static void reset_rq(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);

    // reset rq
    ksym.update_rq_clock(rq);
    rq->avg_idle = 2 * *ksym.sysctl_sched_migration_cost;
    rq->max_idle_balance_cost = *ksym.sysctl_sched_migration_cost;
    rq->nr_switches = 0;
    rq->next_balance = INITIAL_JIFFIES + nsecs_to_jiffies(INIT_TIME_NS);

    // reset cfs rq
    rq->cfs.min_vruntime = INIT_TIME_NS;

// https://github.com/torvalds/linux/commit/af4cf40470c22efa3987200fd19478199e08e103
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    rq->cfs.avg_vruntime = 0;
    rq->cfs.avg_load = 0;
#endif
    memset(&rq->cfs.avg, 0, sizeof(struct sched_avg));
    rq->cfs.avg.last_update_time = INIT_TIME_NS;

    // reset sched domain
    struct sched_domain *sd;
    for_each_domain(rq->cpu, sd) {
      sd->last_balance = jiffies;
      sd->balance_interval = sd->min_interval;
      sd->nr_balance_failed = 0;
    }
  }
}

static void reset_distribute_cpu_mask_prev(void) {
// https://github.com/torvalds/linux/commit/46a87b3851f0d6eb05e6d83d5c5a30df0eca8f76
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    int *ptr = per_cpu_ptr(ksym.distribute_cpu_mask_prev, cpu);
    *ptr = 0;
  }
#endif
}
static void print_all_tasks(void) {
  struct task_struct *p;
  TRACE_INFO("All tasks:");
  for_each_process(p) {
    TRACE_INFO("- pid=%d, cpu=%d, comm=%s", p->pid, task_cpu(p), p->comm);
  }
}

static void run_prog(char *args[]) {
  int ret = call_usermodehelper(args[0], args, NULL, UMH_WAIT_EXEC);
  if (ret < 0) {
    panic("Failed to run %s", args[0]);
  }
}

void kstep_controller_run(struct controller_ops *ops) {
  if (ops->pre_init) {
    ops->pre_init();
  }
  kstep_params_print();
  if (kstep_params.special_topo) {
    kstep_use_special_topo();
  }
  kstep_topo_print();
  kstep_patch_min_vruntime();
  if (kstep_params.print_lb_events) {
    kstep_trace_lb();
  }

  // Isolate the CPUs to avoid interference
  prealloc_kworkers();
  disable_workqueue();
  move_kthreads();

  // Run /cgroup and /busy when we know the system is ready
  run_prog((char *[]){"/cgroup", NULL});
  run_prog((char *[]){"/busy", NULL});

  // Control timer ticks and clock
  kstep_tick_init();

  // Reset the scheduler state to initial state
  reset_rq();
  reset_distribute_cpu_mask_prev();
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) != 0) {
      reset_task_stats(p);
    }
  }

  print_all_tasks();

  TRACE_INFO("Initializing controller %s", ops->name);
  ops->init();
  kstep_sleep();
  TRACE_INFO("Running controller %s", ops->name);
  ops->body();
  TRACE_INFO("Exiting controller %s", ops->name);
  kernel_power_off();

  kstep_tick_exit();
  kstep_trace_exit();
}
