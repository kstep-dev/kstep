#include <linux/cgroup.h>
#include <linux/kernel.h>

#include "op_handler_internal.h"

bool kstep_op_cgroup_is_leaf(int id) {
  for (int i = 0; i < MAX_CGROUPS; i++) {
    if (kstep_cgroups[i].exists && kstep_cgroups[i].parent_id == id)
      return false;
  }
  return true;
}

struct task_group *kstep_op_lookup_cgroup_task_group(const char *name) {
  struct cgroup *cgrp;
  struct cgroup_subsys_state *css;
  struct task_group *tg = NULL;

  cgrp = cgroup_get_from_path(name);
  if (IS_ERR(cgrp))
    return NULL;

  rcu_read_lock();
  css = rcu_dereference(cgrp->subsys[cpu_cgrp_id]);
  if (css)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    tg = css_tg(css);
#else
    tg = container_of(css, struct task_group, css);
#endif
  rcu_read_unlock();
  cgroup_put(cgrp);

  return tg;
}

void kstep_op_cgroup_move_tasks_to_root(int id) {
  for (int i = 0; i < MAX_TASKS; i++) {
    if (!kstep_tasks[i].p || kstep_tasks[i].cgroup_id != id)
      continue;

    TRACE_INFO("Moving task %d from cgroup %d to root",
               kstep_tasks[i].p->pid, id);
    kstep_cgroup_add_task("", kstep_tasks[i].p->pid);
    kstep_task_pin(kstep_tasks[i].p, 1, num_online_cpus() - 1);
    kstep_tasks[i].cgroup_id = -1;
  }
}

void kstep_op_move_task_to_root(int task_id) {
  if (!kstep_op_is_valid_task_id(task_id) || !kstep_tasks[task_id].p)
    panic("Invalid task id %d", task_id);

  TRACE_INFO("Moving task %d from cgroup %d to root",
             kstep_tasks[task_id].p->pid, kstep_tasks[task_id].cgroup_id);
  kstep_cgroup_add_task("", kstep_tasks[task_id].p->pid);
  kstep_task_pin(kstep_tasks[task_id].p, 1, num_online_cpus() - 1);
  kstep_tasks[task_id].cgroup_id = -1;
}

u8 kstep_op_cgroup_create(int a, int b, int c) {
  int parent_id = a;
  int child_id = b;
  char name[MAX_CGROUP_NAME_LEN];

  (void)c;
  if (!kstep_op_is_valid_cgroup_id(child_id) || kstep_cgroups[child_id].exists)
    return 0;
  if (parent_id != -1 && (!kstep_op_is_valid_cgroup_id(parent_id) ||
                          !kstep_cgroups[parent_id].exists))
    return 0;

  kstep_cgroups[child_id].parent_id = parent_id;
  kstep_cgroups[child_id].exists = true;

  if (!kstep_build_cgroup_name(child_id, name))
    return 0;

  if (parent_id != -1)
    kstep_op_cgroup_move_tasks_to_root(parent_id);

  kstep_cgroup_create(name);
  kstep_cgroups[child_id].tg = kstep_op_lookup_cgroup_task_group(name);
  if (!kstep_cgroups[child_id].tg)
    panic("Failed to resolve task group for cgroup %s", name);
  return 1;
}

u8 kstep_op_cgroup_set_cpuset(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  char cpuset[32];

  if (!kstep_op_is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!kstep_build_cgroup_name(a, name))
    return 0;
  if (b > c || b < 1 || c > num_online_cpus() - 1)
    return 0;

  if (scnprintf(cpuset, sizeof(cpuset), "%d-%d", b, c) >= sizeof(cpuset))
    return 0;

  kstep_cgroup_set_cpuset(name, cpuset);
  return 1;
}

u8 kstep_op_cgroup_set_weight(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];

  (void)c;
  if (!kstep_op_is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!kstep_build_cgroup_name(a, name))
    return 0;
  if (b <= 0 || b > 10000)
    return 0;

  kstep_cgroup_set_weight(name, b);
  return 1;
}

u8 kstep_op_cgroup_add_task(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];

  (void)c;
  if (!kstep_op_is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!kstep_build_cgroup_name(a, name))
    return 0;
  if (!kstep_op_is_valid_task_id(b) || !kstep_tasks[b].p)
    return 0;

  if (kstep_tasks[b].p->policy != 0)
    kstep_task_cfs(kstep_tasks[b].p);

  kstep_cgroup_add_task(name, kstep_tasks[b].p->pid);
  kstep_tasks[b].cgroup_id = a;
  return 1;
}

u8 kstep_op_cgroup_destroy(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];

  (void)b;
  (void)c;
  if (!kstep_op_is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!kstep_op_cgroup_is_leaf(a))
    return 0;
  if (!kstep_build_cgroup_name(a, name))
    return 0;

  kstep_op_cgroup_move_tasks_to_root(a);
  kstep_cgroup_destroy(name);
  kstep_cgroups[a].tg = NULL;
  kstep_cgroups[a].exists = false;
  kstep_cgroups[a].parent_id = -1;
  return 1;
}

u8 kstep_op_cgroup_move_task_root(int a, int b, int c) {
  (void)c;

  if (!kstep_op_is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!kstep_op_is_valid_task_id(b) || !kstep_tasks[b].p)
    return 0;
  if (kstep_tasks[b].cgroup_id != a)
    return 0;

  kstep_op_move_task_to_root(b);
  return 1;
}
