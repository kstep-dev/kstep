#include "checker.h"

// https://github.com/torvalds/linux/commit/bbce3de72be56e4b5f68924b7da9630cc89aa1a8
// A dequeue that delays a group entity's parent leaves the parent's slice at U64_MAX (the min
// slice of an empty group); the lag limit derived from it goes negative and the next placement
// throws the entity's vruntime far from its queue, so it is never eligible again. The rule: on
// every level of a session task's hierarchy the slice is sane and the vruntime stays within
// seconds of the queue's min_vruntime.
static void check(struct task_struct *p) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)
  for (struct sched_entity *se = &p->se; se; se = se->parent) {
    struct cfs_rq *cfs_rq = se->cfs_rq;
    s64 lag = (s64)(se->vruntime - cfs_rq->min_vruntime);

    if (se->slice > 100 * NSEC_PER_SEC || lag > 1000 * NSEC_PER_SEC || lag < -1000 * NSEC_PER_SEC)
      return kstep_warn("vruntime", "task %d on cpu %d: slice %llu, vruntime %lld from its queue's min", p->pid,
                        task_cpu(p), se->slice, lag);
  }
#endif
}

static void on_settle(void) {
  int ntasks;
  struct task_struct **tasks = kstep_session_tasks(&ntasks);

  for (int i = 0; i < ntasks; i++)
    if (!tasks[i]->exit_state)
      check(tasks[i]);
}

void kstep_check_vruntime_enable(void) { kstep_on_settle(on_settle); }
