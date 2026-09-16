#include "checker.h"

// Utilization is an average: it decays, it does not fall off a cliff. One command can move a
// task's share of it to another CPU or to another class, and a dying task takes its share with it,
// but nothing else may take more than a task's worth away from a CPU at once. The rule: between
// two commands, a CPU's cfs, rt and total util each fall by less than SCHED_CAPACITY_SCALE, with
// the baseline dropped for a CPU a task has just left or changed class on. Ported from the old
// fuzzer's util checker, which is what found util_avg_jump.
//
// cfs util is read net of the leaf removed.util_avg: a task that has been dequeued elsewhere is
// already subtracted from the rq signal but not yet folded out of the leaves.
struct util_obs {
  s64 cfs, rt;
  bool valid;
};
static DEFINE_PER_CPU(struct util_obs, last_util);
static struct {
  pid_t pid;
  int cpu, policy;
} util_seen[KSTEP_SHM_TASKS];

static s64 cfs_util(struct rq *rq) {
  struct cfs_rq *cfs_rq;
  s64 removed = 0;

  list_for_each_entry_rcu(cfs_rq, &rq->leaf_cfs_rq_list, leaf_cfs_rq_list)
    removed += cfs_rq->removed.util_avg;
  return (s64)rq->cfs.avg.util_avg - (s64)rq->cfs.removed.util_avg - removed;
}

static void on_tick_end(void) {
  int ntasks;
  struct task_struct **tasks = kstep_session_tasks(&ntasks);

  // A task that moved or changed class took its util with it: both CPUs' baselines are void.
  for (int i = 0; i < ntasks && i < KSTEP_SHM_TASKS; i++) {
    struct task_struct *p = tasks[i];

    if (util_seen[i].pid != p->pid) { // a new task in this slot: nothing to compare against
      util_seen[i] = (typeof(util_seen[0])){.pid = p->pid, .cpu = task_cpu(p), .policy = p->policy};
      per_cpu_ptr(&last_util, task_cpu(p))->valid = false;
      continue;
    }
    if (util_seen[i].cpu != task_cpu(p) || util_seen[i].policy != p->policy || p->exit_state) {
      per_cpu_ptr(&last_util, util_seen[i].cpu)->valid = false;
      per_cpu_ptr(&last_util, task_cpu(p))->valid = false;
      util_seen[i].cpu = task_cpu(p);
      util_seen[i].policy = p->policy;
    }
  }

  for_each_test_cpu(cpu) {
    struct rq *rq = cpu_rq(cpu);
    struct util_obs *last = per_cpu_ptr(&last_util, cpu);
    struct util_obs cur = {.cfs = cfs_util(rq), .rt = rq->avg_rt.util_avg, .valid = true};

    if (last->valid &&
        (last->cfs - cur.cfs >= SCHED_CAPACITY_SCALE || last->rt - cur.rt >= SCHED_CAPACITY_SCALE ||
         last->cfs + last->rt - cur.cfs - cur.rt >= SCHED_CAPACITY_SCALE))
      kstep_warn("util_decay", "util on cpu %d fell in one tick: cfs %lld -> %lld, rt %lld -> %lld", cpu, last->cfs,
                 cur.cfs, last->rt, cur.rt);
    *last = cur;
  }
}

void kstep_check_util_decay_enable(void) { kstep_on_tick_end(on_tick_end); }
