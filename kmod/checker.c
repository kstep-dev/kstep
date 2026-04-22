#include <linux/kernel.h>

#include "checker.h"
#include "driver.h"
#include "linux/cpumask.h"
#include "op_handler.h"
#include "op_state.h"

#define for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)                        \
  list_for_each_entry_safe(cfs_rq, pos, &rq->leaf_cfs_rq_list,            \
                           leaf_cfs_rq_list)

typedef void(update_min_vruntime_fn_t)(struct cfs_rq *cfs_rq);

/* Warn when a cgroup weight change leaves the parent vruntime baseline stale. */
static void kstep_check_cgroup_set_weight(int cgroup_id) {
  char name[MAX_CGROUP_NAME_LEN];
  struct task_group *tg;

  if (cgroup_id < 0 || cgroup_id >= MAX_CGROUPS || !kstep_cgroups[cgroup_id].exists)
    return;
  if (!kstep_build_cgroup_name(cgroup_id, name))
    return;

  tg = kstep_cgroups[cgroup_id].tg;
  if (!tg)
    return;

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct sched_entity *se = tg->se[cpu];
    struct cfs_rq *cfs_rq;
    u64 old_min_vruntime, new_min_vruntime;

    if (!se || se->on_rq == 0) continue;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    if (se->on_rq && se->sched_delayed) continue;
#endif 

    cfs_rq = cfs_rq_of(se);
    if (!cfs_rq) continue;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
    KSYM_IMPORT_TYPED(update_min_vruntime_fn_t, avg_vruntime);
    old_min_vruntime = cfs_rq->zero_vruntime;
    KSYM_avg_vruntime(cfs_rq);
    new_min_vruntime = cfs_rq->zero_vruntime;
#else
    KSYM_IMPORT_TYPED(update_min_vruntime_fn_t, update_min_vruntime);
    old_min_vruntime = cfs_rq->min_vruntime;
    KSYM_update_min_vruntime(cfs_rq);
    new_min_vruntime = cfs_rq->min_vruntime;
#endif

    if (new_min_vruntime != old_min_vruntime)
      pr_info("warn: the parent of cgroup %s on cpu%d delayed vruntime update (%llu -> %llu)\n",
              name, cpu, old_min_vruntime, new_min_vruntime);
    
  }
}

/* Log a warning when runnable work could be placed on an idle CPU right now. */
void kstep_check_work_conserve(void) {
  struct cpumask idle_cpus;
  int runnable_tasks = 0;
  bool eligible_runnable = false;

  cpumask_clear(&idle_cpus);
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    if (cpu_rq(cpu)->nr_running == 0)
      cpumask_set_cpu(cpu, &idle_cpus);
  }

  for (int i = 0; i < MAX_TASKS; i++) {
    struct task_struct *p = kstep_tasks[i].p;
    if (!p || p->__state != TASK_RUNNING)
      continue;
    runnable_tasks++;
    // If the task is on a busy CPU and is eligible to move to the idle cpu
    if (cpumask_intersects(p->cpus_ptr, &idle_cpus) &&
        task_rq(p)->nr_running > 1)
      eligible_runnable = true;
  }

  if (runnable_tasks > num_online_cpus() - 1 &&
      !cpumask_empty(&idle_cpus) && 
      eligible_runnable) {
    pr_info("warn: work conserving violation runnable=%d idle_cpus=%*pbl eligible=%d\n",
            runnable_tasks, cpumask_pr_args(&idle_cpus), eligible_runnable);
  }
}

/* Log the selected balance decision and flag obviously unnecessary balancing
 * inside the local sched group. */
void kstep_check_extra_balance(int cpu, struct sched_domain *sd) {
  int i;
  struct sched_group *sg = sd->groups;
  kstep_output_balance(cpu, sd);
  if (cpu_rq(cpu)->nr_running == 0)
    return;
  do {
    // Find the local group
    if (!cpumask_test_cpu(cpu, sched_group_span(sg)))
      continue;

    for_each_cpu(i, sched_group_span(sg)) {
      if (cpu_rq(i)->nr_running == 0) {
        pr_info("warn: load balance triggered on busy cpu while idle cpu in the same group");
        return;
      }
    }
  } while (sg != sd->groups);
}


static s64 get_cfs_util_avg(struct rq *rq) {
  s64 removed = 0;
  struct cfs_rq *cfs_rq, *pos;

  /* Match the rq-visible util signal after subtracting per-leaf removed load
   * that has not been folded back yet. */
  for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)
    removed += cfs_rq->removed.util_avg;

  return rq->cfs.avg.util_avg - rq->cfs.removed.util_avg - removed;
}

static s64 get_rt_util_avg(struct rq *rq) {
  return rq->avg_rt.util_avg;
}

/* Capture the per-CPU util baselines that the post-op checker compares
 * against. */
void kstep_check_before_op(struct kstep_check_state *check) {
  memset(check, 0, sizeof(*check));
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    check->cfs_util_avg[cpu] = get_cfs_util_avg(rq);
    check->rt_util_avg[cpu] = get_rt_util_avg(rq);
  }
}

/* Update the saved baselines for legal accounting moves, then emit warnings
 * for unexpectedly large util drops caused by the op. */
void kstep_check_after_op(struct kstep_check_state *check,
                          enum kstep_op_type type, int a, int b, int c) {
  (void)b;
  (void)c;

  for (int i = 0; i < MAX_TASKS; i++) {
    struct task_struct *p = kstep_tasks[i].p;

    if (!p)
      continue;

    /* Migration or scheduler-class changes legitimately move util accounting,
     * so discard the saved baseline for the old CPU before comparing. */
    if (kstep_tasks[i].cur_cpu != task_cpu(p)) {
      check->cfs_util_avg[kstep_tasks[i].cur_cpu] = 0;
      kstep_tasks[i].cur_cpu = task_cpu(p);
    }
    if (kstep_tasks[i].cur_policy != p->policy) {
      check->cfs_util_avg[kstep_tasks[i].cur_cpu] = 0;
      kstep_tasks[i].cur_policy = p->policy;
    }
  }

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);

    if (check->cfs_util_avg[cpu] - get_cfs_util_avg(rq) > 1010) {
      pr_info("warn: cfs_util_avg violation on cpu %d\n", cpu);
    } else if (check->rt_util_avg[cpu] - get_rt_util_avg(rq) > 1010) {
      pr_info("warn: rt_util_avg violation on cpu %d\n", cpu);
    } else if (check->rt_util_avg[cpu] + check->cfs_util_avg[cpu] -
               get_cfs_util_avg(rq) - get_rt_util_avg(rq) > 1010) {
      pr_info("warn: total_util_avg violation on cpu %d\n", cpu);
    }
  }

  if (type == OP_CGROUP_SET_WEIGHT)
    kstep_check_cgroup_set_weight(a);
}
