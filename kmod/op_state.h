#ifndef KSTEP_OP_STATE_H
#define KSTEP_OP_STATE_H

#include "internal.h"

#define MAX_TASKS 1024
#define MAX_CGROUPS 1024
#define MAX_CGROUP_NAME_LEN 256

struct kstep_task {
  struct task_struct *p;
  int cgroup_id;
  int cur_cpu;
  int cur_policy; // 0: cfs, 1: rt
};

struct kstep_managed_kthread {
  struct task_struct *p;
};

struct kstep_cgroup_state {
  bool exists;
  int parent_id;
  struct task_group *tg;
};

extern struct kstep_task kstep_tasks[MAX_TASKS];
extern struct kstep_managed_kthread kstep_kthreads[KSTEP_MAX_KTHREADS];
extern struct kstep_cgroup_state kstep_cgroups[MAX_CGROUPS];
extern int cgroup_lineage[MAX_CGROUPS];

bool kstep_build_cgroup_name(int id, char *buf);

#endif
