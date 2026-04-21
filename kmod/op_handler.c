#include <linux/cgroup.h>
#include <linux/kernel.h> // panic, scnprintf
#include <linux/sched/signal.h> // for_each_process
#include <linux/string.h> // strcmp, strscpy

#include "checker.h"
#include "driver.h"
#include "op_state.h"
#include "op_handler.h"
#include "linux/cpumask.h"
#include "linux/sched.h"

struct kstep_task kstep_tasks[MAX_TASKS];
struct kstep_managed_kthread kstep_kthreads[KSTEP_MAX_KTHREADS];
struct kstep_cgroup_state kstep_cgroups[MAX_CGROUPS];
int cgroup_lineage[MAX_CGROUPS];

static bool is_valid_task_id(int id) { return id >= 0 && id < MAX_TASKS; }
static bool is_valid_cgroup_id(int id) { return id >= 0 && id < MAX_CGROUPS; }
static bool is_valid_kthread_id(int id) {
  return id >= 0 && id < KSTEP_MAX_KTHREADS;
}

static bool build_cgroup_name(int id, char *buf) {
  int depth = 0;
  int cur = id;
  int len = 0;

  while (cur != -1) {
    if (!is_valid_cgroup_id(cur) || !kstep_cgroups[cur].exists ||
        depth >= MAX_CGROUPS)
      return false;
    cgroup_lineage[depth++] = cur;
    cur = kstep_cgroups[cur].parent_id;
  }

  for (int i = depth - 1; i >= 0; i--) {
    len += scnprintf(buf + len, MAX_CGROUP_NAME_LEN - len, 
                "cg%d%s", cgroup_lineage[i], (i > 0) ? "/" : "");
    if (len >= MAX_CGROUP_NAME_LEN)
      return false;
  }
  return true;
}

static bool cgroup_is_leaf(int id) {
  for (int i = 0; i < MAX_CGROUPS; i++) {
    if (kstep_cgroups[i].exists && kstep_cgroups[i].parent_id == id)
      return false;
  }
  return true;
}

static struct task_group *lookup_cgroup_task_group(const char *name) {
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

static void cgroup_move_tasks_to_root(int id) {
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

static void move_task_to_root(int task_id) {
  if (!is_valid_task_id(task_id) || !kstep_tasks[task_id].p)
    panic("Invalid task id %d", task_id);

  TRACE_INFO("Moving task %d from cgroup %d to root",
             kstep_tasks[task_id].p->pid, kstep_tasks[task_id].cgroup_id);
  kstep_cgroup_add_task("", kstep_tasks[task_id].p->pid);
  kstep_task_pin(kstep_tasks[task_id].p, 1, num_online_cpus() - 1);
  kstep_tasks[task_id].cgroup_id = -1;
}

static bool pid_known(pid_t pid) {
  for (int i = 0; i < MAX_TASKS; i++) {
    if (kstep_tasks[i].p && kstep_tasks[i].p->pid == pid)
      return true;
  }
  return false;
}

static bool kstep_task_running(struct task_struct * p) {
#ifdef TIF_NEED_RESCHED_LAZY
  return p->on_cpu && !test_tsk_thread_flag(p, TIF_NEED_RESCHED_LAZY) && !test_tsk_thread_flag(p, TIF_NEED_RESCHED);
#else
  return p->on_cpu && !test_tsk_thread_flag(p, TIF_NEED_RESCHED);
#endif
}

static enum kstep_kthread_state kthread_state(int id) {
  if (!is_valid_kthread_id(id) || !kstep_kthreads[id].p)
    return KSTEP_KTHREAD_DEAD;
  return kstep_kthread_get_state(kstep_kthreads[id].p);
}

static struct task_struct *find_new_child(struct task_struct *parent) {
  struct task_struct *p;
  for (int attempt = 0; attempt < 100; attempt++) {
    for_each_process(p) {
      if ((p->real_parent == parent || p->parent == parent) &&
          // strcmp(p->comm, TASK_READY_COMM) == 0 && 
          p->pid > parent->pid &&
          !pid_known(p->pid))
        return p;
    }
    kstep_sleep();
  }
    panic("No new child found for parent %d", parent->pid);
}

static u8 op_task_create(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || kstep_tasks[a].p)
    panic("Invalid task id");
  kstep_tasks[a].p = kstep_task_create();
  kstep_tasks[a].cgroup_id = -1;
  return 1;
}

static u8 op_task_fork(int a, int b, int c) {
  struct task_struct *p;
  (void)c;

  if (!is_valid_task_id(a) || !is_valid_task_id(b))
    return 0;
  if (!kstep_tasks[a].p || kstep_tasks[b].p)
    return 0;

  if (!kstep_task_running(kstep_tasks[a].p))
      panic("Task %d is not on CPU when forking", a);
  kstep_task_fork(kstep_tasks[a].p, 1);
  p = find_new_child(kstep_tasks[a].p);
  if (!p)
    return 0;

  kstep_tasks[b].p = p;
  kstep_tasks[b].cgroup_id = kstep_tasks[a].cgroup_id;
  return 1;
}

static u8 op_task_pin(int a, int b, int c) {
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when pinning", a);
  kstep_task_pin(kstep_tasks[a].p, b, c);
  return 1;
}

static u8 op_task_fifo(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;

  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting FIFO", a);
  // Move the task back to the root cgroup, otherwise the set_schedprio will fail
  move_task_to_root(a);
  kstep_task_fifo(kstep_tasks[a].p);
  return 1;
}

static u8 op_task_cfs(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting CFS", a);
  kstep_task_cfs(kstep_tasks[a].p);
  return 1;
}

static u8 op_task_pause(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when pausing", a);
  kstep_task_pause(kstep_tasks[a].p);
  return 1;
}

static u8 op_task_wakeup(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (kstep_tasks[a].p->__state == TASK_RUNNING)
    panic("Task %d is already on CPU when waking up", a);
  kstep_task_wakeup(kstep_tasks[a].p);
  return 1;
}

static u8 op_task_set_prio(int a, int b, int c) {
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting priority", a);
  if (b < -20 || b > 19)
    return 0;
  kstep_task_set_prio(kstep_tasks[a].p, b);
  return 1;
}

static u8 op_kthread_create(int a, int b, int c) {
  char name[16];

  (void)b;
  (void)c;
  if (!is_valid_kthread_id(a) || kstep_kthreads[a].p)
    return 0;

  scnprintf(name, sizeof(name), "kt%d", a);
  kstep_kthreads[a].p = kstep_kthread_create(name);
  return 1;
}

static u8 op_kthread_bind(int a, int b, int c) {
  struct cpumask mask;
  enum kstep_kthread_state state = kthread_state(a);

  if (!is_valid_kthread_id(a) || !kstep_kthreads[a].p ||
      state == KSTEP_KTHREAD_DEAD)
    return 0;
  if (b > c || b < 1 || c > num_online_cpus() - 1)
    return 0;

  cpumask_clear(&mask);
  for (int cpu = b; cpu <= c; cpu++)
    cpumask_set_cpu(cpu, &mask);
  kstep_kthread_bind(kstep_kthreads[a].p, &mask);
  return 1;
}

static u8 op_kthread_start(int a, int b, int c) {
  (void)b;
  (void)c;

  if (!is_valid_kthread_id(a) || !kstep_kthreads[a].p)
    return 0;
  if (kthread_state(a) != KSTEP_KTHREAD_CREATED)
    return 0;
  kstep_kthread_start(kstep_kthreads[a].p);
  return 1;
}

static u8 op_kthread_yield(int a, int b, int c) {
  enum kstep_kthread_state state;

  (void)b;
  (void)c;
  if (!is_valid_kthread_id(a) || !kstep_kthreads[a].p)
    return 0;
  state = kthread_state(a);
  if (state != KSTEP_KTHREAD_SPIN && state != KSTEP_KTHREAD_YIELD)
    return 0;
  kstep_kthread_yield(kstep_kthreads[a].p);
  return 1;
}

static u8 op_kthread_block(int a, int b, int c) {
  enum kstep_kthread_state state;

  (void)b;
  (void)c;
  if (!is_valid_kthread_id(a) || !kstep_kthreads[a].p)
    return 0;
  state = kthread_state(a);
  if (state != KSTEP_KTHREAD_SPIN && state != KSTEP_KTHREAD_YIELD)
    return 0;
  kstep_kthread_block(kstep_kthreads[a].p);
  return 1;
}

static u8 op_kthread_syncwake(int a, int b, int c) {
  enum kstep_kthread_state waker_state;
  enum kstep_kthread_state wakee_state;

  (void)c;
  if (!is_valid_kthread_id(a) || !is_valid_kthread_id(b))
    return 0;
  if (a == b)
    return 0;

  waker_state = kthread_state(a);
  wakee_state = kthread_state(b);
  if (!kstep_kthreads[a].p || !kstep_kthreads[b].p)
    return 0;
  if (waker_state != KSTEP_KTHREAD_SPIN &&
      waker_state != KSTEP_KTHREAD_YIELD)
    return 0;
  if (wakee_state != KSTEP_KTHREAD_BLOCKED)
    return 0;

  kstep_kthread_syncwake(kstep_kthreads[a].p, kstep_kthreads[b].p);
  return 1;
}

static u8 op_tick(int a, int b, int c) {
  (void)a;
  (void)b;
  (void)c;
  kstep_tick();
  return 1;
}

static u64 count_ineligible_cgroup_se(void) {
  u64 count = 0;

  for (int id = 0; id < MAX_CGROUPS; id++) {
    struct task_group *tg;

    if (!kstep_cgroups[id].exists)
      continue;

    tg = kstep_cgroups[id].tg;
    if (!tg)
      continue;

    for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
      struct sched_entity *se = tg->se[cpu];

      if (se && !kstep_eligible(se))
        count++;
    }
  }

  return count;
}

static inline struct sched_entity *node_to_se(struct rb_node *node) {
  return rb_entry(node, struct sched_entity, run_node);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static bool cfs_rq_tree_has_eligible_entity(struct rb_node *node) {
  struct sched_entity *se;

  if (!node)
    return false;

  se = node_to_se(node);
  if (se->on_rq && kstep_eligible(se))
    return true;

  return cfs_rq_tree_has_eligible_entity(node->rb_left) ||
         cfs_rq_tree_has_eligible_entity(node->rb_right);

  return true;
}
# endif

static bool cfs_rq_nonempty_with_zero_eligible(struct cfs_rq *cfs_rq) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  struct sched_entity *curr;
  bool curr_present;

  if (!cfs_rq)
    return false;

  curr = cfs_rq->curr;
  curr_present = curr && curr->cfs_rq == cfs_rq && curr->on_rq;
  if (!curr_present && cfs_rq->nr_queued == 0)
    return false;

  if (curr_present && kstep_eligible(curr))
    return false;

  return !cfs_rq_tree_has_eligible_entity(cfs_rq->tasks_timeline.rb_root.rb_node);
# endif
  return false;
}

static bool has_nonempty_cfs_rq_with_zero_eligible(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    if (cfs_rq_nonempty_with_zero_eligible(&cpu_rq(cpu)->cfs))
      return true;
  }

  for (int id = 0; id < MAX_CGROUPS; id++) {
    struct task_group *tg;

    if (!kstep_cgroups[id].exists)
      continue;

    tg = kstep_cgroups[id].tg;
    if (!tg)
      continue;

    for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
      if (cfs_rq_nonempty_with_zero_eligible(tg->cfs_rq[cpu]))
        return true;
    }
  }

  return false;
}

static u8 op_tick_repeat(int a, int b, int c) {
  u8 executed_steps = 0;
  (void)b;
  (void)c;

  for (int i = 0; i < a; i++) {
    kstep_execute_op(OP_TICK, 0, 0, 0);

    /*
      Some bugs require special task/task group states to trigger. 
      These conditions are hard to capture with code coverage, 
      and sensitive to time (i.e. how many ticks invoked).
      Break if special state found
      These states come from studied bug set.
    */
    if (count_ineligible_cgroup_se() > (num_online_cpus() - 1))
      break;
    if (has_nonempty_cfs_rq_with_zero_eligible()) {
      TRACE_INFO("Found nonempty cfs with zero eligible");
      break;
    }
    executed_steps++;
  }

  return executed_steps;
}

static u8 op_cgroup_create(int a, int b, int c) {
  int parent_id = a;
  int child_id = b;
  char name[MAX_CGROUP_NAME_LEN];
  (void)c;

  if (!is_valid_cgroup_id(child_id) || kstep_cgroups[child_id].exists)
    return 0;
  if (parent_id != -1 &&
      (!is_valid_cgroup_id(parent_id) || !kstep_cgroups[parent_id].exists))
    return 0;

  kstep_cgroups[child_id].parent_id = parent_id;
  kstep_cgroups[child_id].exists = true;

  if (!build_cgroup_name(child_id, name))
    return 0;

  // Only leaf cgroups can contain tasks in cgroup v2.
  if (parent_id != -1)
    cgroup_move_tasks_to_root(parent_id);

  kstep_cgroup_create(name);
  kstep_cgroups[child_id].tg = lookup_cgroup_task_group(name);
  if (!kstep_cgroups[child_id].tg)
    panic("Failed to resolve task group for cgroup %s", name);
  return 1;
}

static u8 op_cgroup_set_cpuset(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  char cpuset[32];

  if (!is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!build_cgroup_name(a, name))
    return 0;
  if (b > c || b < 1 || c > num_online_cpus() - 1)
    return 0;

  if (scnprintf(cpuset, sizeof(cpuset), "%d-%d", b, c) >= sizeof(cpuset))
    return 0;

  kstep_cgroup_set_cpuset(name, cpuset);
  return 1;
}

typedef void(update_min_vruntime_fn_t)(struct cfs_rq *cfs_rq);

static u8 op_cgroup_set_weight(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  int cpu;
  struct task_group *tg;
  (void)c;
  


  if (!is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!build_cgroup_name(a, name))
    return 0;
  if (b <= 0 || b > 10000)
    return 0;

  kstep_cgroup_set_weight(name, b);

  // check whether the min_vruntime has been updated in time
  tg = kstep_cgroups[a].tg;
  if (!tg)
    return 0;

  for (cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct sched_entity *se = tg->se[cpu];
    struct cfs_rq *cfs_rq;
    u64 old_min_vruntime;
    u64 new_min_vruntime;

    if (!se)
      continue;

    cfs_rq = cfs_rq_of(se);
    if (!cfs_rq)
      continue;

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

    if (new_min_vruntime != old_min_vruntime) {
      pr_info("warn: the parent of cgroup %s on cpu%d delayed vruntime update (%llu -> %llu)\n",
              name, cpu, old_min_vruntime, new_min_vruntime);
    }
  }

  return 1;
}

static u8 op_cgroup_add_task(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  (void)c;

  if (!is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!build_cgroup_name(a, name))
    return 0;
  if (!is_valid_task_id(b) || !kstep_tasks[b].p)
    return 0;

  if (kstep_tasks[b].p->policy != 0)
    kstep_task_cfs(kstep_tasks[b].p);
  
  kstep_cgroup_add_task(name, kstep_tasks[b].p->pid);

  kstep_tasks[b].cgroup_id = a;
  return 1;
}

static u8 op_cgroup_destroy(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  (void)b;
  (void)c;

  if (!is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!cgroup_is_leaf(a))
    return 0;
  if (!build_cgroup_name(a, name))
    return 0;

  cgroup_move_tasks_to_root(a);
  kstep_cgroup_destroy(name);
  kstep_cgroups[a].tg = NULL;
  kstep_cgroups[a].exists = false;
  kstep_cgroups[a].parent_id = -1;
  return 1;
}

static u8 op_cgroup_move_task_root(int a, int b, int c) {
  (void)c;

  if (!is_valid_cgroup_id(a) || !kstep_cgroups[a].exists)
    return 0;
  if (!is_valid_task_id(b) || !kstep_tasks[b].p)
    return 0;
  if (kstep_tasks[b].cgroup_id != a)
    return 0;

  move_task_to_root(b);
  return 1;
}

typedef u8 (*op_handler_fn)(int a, int b, int c);

static op_handler_fn op_handlers[OP_TYPE_NR] = {
    [OP_TASK_CREATE] = op_task_create,
    [OP_TASK_FORK] = op_task_fork,
    [OP_TASK_PIN] = op_task_pin,
    [OP_TASK_FIFO] = op_task_fifo,
    [OP_TASK_CFS] = op_task_cfs,
    [OP_TASK_PAUSE] = op_task_pause,
    [OP_TASK_WAKEUP] = op_task_wakeup,
    [OP_TASK_SET_PRIO] = op_task_set_prio,
    [OP_TICK] = op_tick,
    [OP_TICK_REPEAT] = op_tick_repeat,
    [OP_CGROUP_CREATE] = op_cgroup_create,
    [OP_CGROUP_SET_CPUSET] = op_cgroup_set_cpuset,
    [OP_CGROUP_SET_WEIGHT] = op_cgroup_set_weight,
    [OP_CGROUP_ADD_TASK] = op_cgroup_add_task,
    [OP_CPU_SET_FREQ] = NULL,
    [OP_CPU_SET_CAPACITY] = NULL,
    [OP_CGROUP_DESTROY] = op_cgroup_destroy,
    [OP_CGROUP_MOVE_TASK_ROOT] = op_cgroup_move_task_root,
    [OP_KTHREAD_CREATE] = op_kthread_create,
    [OP_KTHREAD_BIND] = op_kthread_bind,
    [OP_KTHREAD_START] = op_kthread_start,
    [OP_KTHREAD_YIELD] = op_kthread_yield,
    [OP_KTHREAD_BLOCK] = op_kthread_block,
    [OP_KTHREAD_SYNCWAKE] = op_kthread_syncwake,
};

static const char op_strs[OP_TYPE_NR][30] = {
  [OP_TASK_CREATE] = "TASK_CREATE",
  [OP_TASK_FORK] = "TASK_FORK",
  [OP_TASK_PIN] = "TASK_PIN",
  [OP_TASK_FIFO] = "TASK_FIFO",
  [OP_TASK_CFS] = "TASK_CFS",
  [OP_TASK_PAUSE] = "TASK_PAUSE",
  [OP_TASK_WAKEUP] = "TASK_WAKEUP",
  [OP_TASK_SET_PRIO] = "TASK_SET_PRIO",
  [OP_TICK] = "TICK",
  [OP_TICK_REPEAT] = "TICK_REPEAT",
  [OP_CGROUP_CREATE] = "CGROUP_CREATE",
  [OP_CGROUP_SET_CPUSET] = "CGROUP_SET_CPUSET",
  [OP_CGROUP_SET_WEIGHT] = "CGROUP_SET_WEIGHT",
  [OP_CGROUP_ADD_TASK] = "CGROUP_ADD_TASK",
  [OP_CPU_SET_FREQ] = "CPU_SET_FREQ",
  [OP_CPU_SET_CAPACITY] = "CPU_SET_CAPACITY",
  [OP_CGROUP_DESTROY] = "CGROUP_DESTROY",
  [OP_CGROUP_MOVE_TASK_ROOT] = "CGROUP_MOVE_TASK_ROOT",
  [OP_KTHREAD_CREATE] = "KTHREAD_CREATE",
  [OP_KTHREAD_BIND] = "KTHREAD_BIND",
  [OP_KTHREAD_START] = "KTHREAD_START",
  [OP_KTHREAD_YIELD] = "KTHREAD_YIELD",
  [OP_KTHREAD_BLOCK] = "KTHREAD_BLOCK",
  [OP_KTHREAD_SYNCWAKE] = "KTHREAD_SYNCWAKE",
};


/* Returns true if task p is in the state required to receive op type. */
static bool op_task_state_ready(enum kstep_op_type type, struct task_struct *p) {
  if (type == OP_TASK_WAKEUP)
    return p->__state != TASK_RUNNING;           /* task must be blocked/dequeued */
  return kstep_task_running(p);             /* all other signal ops need on-cpu */
}

static bool is_task_signal_op(enum kstep_op_type type) {
  switch (type) {
  case OP_TASK_FORK: case OP_TASK_PIN:  case OP_TASK_FIFO:
  case OP_TASK_CFS:  case OP_TASK_PAUSE: case OP_TASK_WAKEUP:
  case OP_TASK_SET_PRIO: return true;
  default: return false;
  }
}

static void print_cgroup_state(int id) {
  char name[MAX_CGROUP_NAME_LEN];
  char eligible[256] = "";
  int len = 0;
  struct task_group *tg = kstep_cgroups[id].tg;

  if (!build_cgroup_name(id, name))
    return;

  if (!tg) {
    pr_info("cgroup %d %s: eligible=unavailable\n", id, name);
    return;
  }

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct sched_entity *se = tg->se[cpu];

    if (!se)
      continue;

    len += scnprintf(eligible + len, sizeof(eligible) - len, "%scpu%d=%d",
                     len ? " " : "", cpu, kstep_eligible(se));
    if (len >= sizeof(eligible))
      break;
  }

  pr_info("cgroup %d %s: eligible={%s}\n", id, name,
          eligible[0] ? eligible : "n/a");
}

static void print_state(void) {
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task_struct *p = kstep_tasks[i].p;
    if (!p)
      continue;
    pr_info("Task %d %d: on_cpu=%d, state=%d, cpu=%d, class=%d, util_avg=%lu, eligible=%d\n", i, 
             p->pid, kstep_task_running(p), p->__state, task_cpu(p), p->policy, p->se.avg.util_avg, kstep_eligible(&p->se));
  }

  for (int i = 0; i < KSTEP_MAX_KTHREADS; i++) {
    enum kstep_kthread_state state;

    if (!kstep_kthreads[i].p)
      continue;
    state = kthread_state(i);
    pr_info("Kthread %d: pid=%d, cpu=%d, state=%d\n", i,
            state == KSTEP_KTHREAD_DEAD ? -1 : kstep_kthreads[i].p->pid, task_cpu(kstep_kthreads[i].p), kstep_kthreads[i].p->__state);
  }

  for (int i = 0; i < MAX_CGROUPS; i++) {
    if (!kstep_cgroups[i].exists)
      continue;
    print_cgroup_state(i);
  }
}

/*
 * Try to send the head queued op for task @id if its required state is met.
 * Sends at most one op per call.
 */
static bool skip_global_cov(enum kstep_op_type type) {
  switch (type) {
  case OP_KTHREAD_CREATE: case OP_KTHREAD_START: case OP_KTHREAD_BIND:
  case OP_KTHREAD_YIELD: case OP_KTHREAD_BLOCK: case OP_KTHREAD_SYNCWAKE:
    return true;
  default:
    return false;
  }
}

static u8 execute_one_op(enum kstep_op_type type, int a, int b, int c) {
  struct kstep_check_state check_state;
  bool collect_cov = !skip_global_cov(type);
  u8 executed_steps;

  kstep_check_before_op(&check_state);

  /*
   * START/YIELD hand control to a live remote kthread that can keep running
   * during the helper's trailing sleep. Global coverage in that window floods
   * the async kthread path rather than the command itself.
  */
  if (collect_cov)
    kstep_cov_enable();
  pr_info("EXECOP: {\"op\": %d, \"a\": %d, \"b\": %d, \"c\": %d}\n", type, a, b, c);
  executed_steps = op_handlers[type](a, b, c);
  if (executed_steps == 0) {
    if (collect_cov)
      kstep_cov_disable();
    // panic("Operation failed: %s %d %d %d\n", op_strs[type], a, b, c);
    return 0;
  }
  if (collect_cov)
    kstep_cov_disable();
  kstep_cov_dump();
  kstep_cov_cmd_id_inc();
  print_state();
  kstep_check_after_op(&check_state);

  return executed_steps;
}

static u8 buf[1 + 1 + 1 + MAX_TASKS * 2 + 1 + KSTEP_MAX_KTHREADS * 2 + 1];

static u8 encode_state_byte(u8 val) {
  return val + 11; /* avoid '\n' in the binary frame */
}

void kstep_write_state(struct file *f, u8 executed_steps) {
  /* Format:
   * [OP_TYPE_NR] [executed_steps+11]
   * [task_id] [task_state] ... [0]
   * [kthread_id] [kthread_state] ... ['\n']
   *
   * Task ids and kthread ids are offset by 11 to avoid '\n'. The zero byte
   * terminates the task section; the rest of the payload is the kthread
   * section. Task states are 0=blocked, 1=runnable, 2=on_cpu. */
  loff_t pos = 0;
  int len = 0;
  

  buf[len++] = OP_TYPE_NR;
  buf[len++] = encode_state_byte(executed_steps);
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task_struct *p = kstep_tasks[i].p;
    if (!p)
      continue;
    u8 state;
    if (kstep_task_running(p))
      state = 2;
    else if (p->__state == TASK_RUNNING)
      state = 1;
    else
      state = 0;
    buf[len++] = (u8)i + 11; // avoid writing 10 (\n)
    buf[len++] = state;
  }
  buf[len++] = 0;
  for (int i = 0; i < KSTEP_MAX_KTHREADS; i++) {
    enum kstep_kthread_state state;

    if (!kstep_kthreads[i].p)
      continue;
    state = kstep_kthread_get_state(kstep_kthreads[i].p);
    buf[len++] = (u8)i + 11;
    buf[len++] = (u8)state;
    if (state == KSTEP_KTHREAD_DEAD)
      kstep_kthreads[i].p = NULL;
  }
  buf[len++] = '\n';
  kernel_write(f, buf, len, &pos);
}

u8 kstep_execute_op(enum kstep_op_type type, int a, int b, int c) {
  if (type < 0 || type >= OP_TYPE_NR)
    panic("Operation failed: %d %d %d %d\n", type, a, b, c);
  if (!op_handlers[type]) {
    TRACE_INFO("Operation not implemented: %d\n", type);
    return 0;
  }

  if (type == OP_TICK_REPEAT)
    return op_handlers[type](a, b, c);

  /*
   * Signal ops are serialised through the per-task queue.
   * Rule: enqueue if the queue is already non-empty (another op is waiting)
   *       OR the task is not yet in the required state.
   *       Send directly only when queue is empty AND state is ready.
   */
  if (is_task_signal_op(type)) {
    if (!is_valid_task_id(a) || !kstep_tasks[a].p)
      panic("Task %d not found", a);
    if (!op_task_state_ready(type, kstep_tasks[a].p)) {
      // directly return when task is not in the required state
      // then, the executor will send the latest state to the input generator
      TRACE_INFO("Task %d is not in the required state for operation %s", a, op_strs[type]);
      return 0;
    }
  }

  return execute_one_op(type, a, b, c);
}
