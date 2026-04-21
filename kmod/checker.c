#include <linux/kernel.h>

#include "checker.h"
#include "driver.h"
#include "linux/cpumask.h"
#include "op_handler.h"
#include "op_state.h"

#define for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)                        \
  list_for_each_entry_safe(cfs_rq, pos, &rq->leaf_cfs_rq_list,            \
                           leaf_cfs_rq_list)

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
void kstep_check_after_op(struct kstep_check_state *check) {
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
}
