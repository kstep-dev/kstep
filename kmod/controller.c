#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/reboot.h>

#include "kstep.h"

extern struct controller_ops controller_aa3ee4f;
extern struct controller_ops controller_bbce3de;
extern struct controller_ops controller_cd9626e;
extern struct controller_ops controller_2feab24;
extern struct controller_ops controller_17e3e88;
extern struct controller_ops controller_5068d84;
extern struct controller_ops controller_evenIdleCpu;
extern struct controller_ops controller_6d7e478;
extern struct controller_ops controller_case_time_sensitive;
extern struct controller_ops controller_noop;

static struct controller_ops *controller_ops_list[] = {
    &controller_aa3ee4f,
    &controller_bbce3de,
    &controller_cd9626e,
    &controller_2feab24,
    &controller_17e3e88,
    &controller_5068d84,
    &controller_evenIdleCpu,
    &controller_6d7e478,
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

void kstep_sleep(void) { udelay(kstep_params.step_interval_us); }

void call_tick_once(void) {
  if (kstep_params.print_tasks) {
    print_tasks();
  }
  if (kstep_params.print_nr_running) {
    print_nr_running();
  }
  kstep_clock_tick();

  // Call tick function
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
    smp_call_function_single(cpu, (void *)ksym.sched_tick, NULL, 0);
#else
    smp_call_function_single(cpu, (void *)ksym.scheduler_tick, NULL, 0);
#endif
    kstep_sleep();
  }
}

static void disable_timer_ticks(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    // Ref: tick_sched_timer_dying in
    // https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-sched.c#L1606
    struct tick_sched *ts = ksym.tick_get_tick_sched(cpu);
    hrtimer_cancel(&ts->sched_timer);
    memset(ts, 0, sizeof(struct tick_sched));
    TRACE_INFO("Disabled timer ticks on CPU %d", cpu);
  }
}

static void enable_timer_ticks(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.tick_setup_sched_timer,
                             (void *)true, 0);
  }
}

static void disable_workqueue(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.workqueue_offline_cpu,
                             (void *)(uintptr_t)cpu, 1);
    TRACE_INFO("Disabled workqueue on CPU %d", cpu);
  }
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

  // Isolate the CPUs to avoid interference
  disable_workqueue();
  move_kthreads();

  // Control timer ticks and clock
  disable_timer_ticks();
  kstep_clock_init(INIT_TIME_NS);

  // Reset the scheduler state to initial state
  reset_rq();
  reset_distribute_cpu_mask_prev();

  TRACE_INFO("Initializing controller %s", ops->name);
  ops->init();
  kstep_sleep();
  TRACE_INFO("Running controller %s", ops->name);
  ops->body();
  TRACE_INFO("Exiting controller %s", ops->name);
  kernel_power_off();

  kstep_clock_exit();
  enable_timer_ticks();
  kstep_trace_exit();
}
