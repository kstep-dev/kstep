#include <linux/cgroup.h>
#include <linux/kernel.h> // panic, scnprintf
#include <linux/sched/signal.h> // for_each_process
#include <linux/string.h> // strcmp, strscpy

#include "driver.h"
#include "internal.h"
#include "op_handler.h"
#include "linux/cpumask.h"
#include "linux/sched.h"

#define MAX_TASKS 1024
#define MAX_CGROUPS 1024
#define MAX_CGROUP_NAME_LEN 256

static int cgroup_parent_id[MAX_CGROUPS];
static bool cgroup_exists[MAX_CGROUPS];
static int cgroup_lineage[MAX_CGROUPS];
static struct task_group *cgroup_tg[MAX_CGROUPS];

#define TASK_OP_QUEUE_SIZE 64

struct queued_op { enum kstep_op_type type; int a, b, c; u64 seq; };

struct kstep_task {
  struct task_struct *p;
  int cgroup_id;
  int cur_cpu;
  int cur_policy; // 0: cfs, 1: rt
};

static struct kstep_task kstep_tasks[MAX_TASKS];
static u8 last_executed_steps;

static bool is_valid_task_id(int id) { return id >= 0 && id < MAX_TASKS; }
static bool is_valid_cgroup_id(int id) { return id >= 0 && id < MAX_CGROUPS; }

static bool build_cgroup_name(int id, char *buf) {
  int depth = 0;
  int cur = id;
  int len = 0;

  while (cur != -1) {
    if (!is_valid_cgroup_id(cur) || !cgroup_exists[cur] || depth >= MAX_CGROUPS)
      return false;
    cgroup_lineage[depth++] = cur;
    cur = cgroup_parent_id[cur];
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
    if (cgroup_exists[i] && cgroup_parent_id[i] == id)
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

static bool op_task_create(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || kstep_tasks[a].p)
    panic("Invalid task id");
  kstep_tasks[a].p = kstep_task_create();
  kstep_tasks[a].cgroup_id = -1;
  return true;
}

static bool op_task_fork(int a, int b, int c) {
  struct task_struct *p;
  (void)c;

  if (!is_valid_task_id(a) || !is_valid_task_id(b))
    return false;
  if (!kstep_tasks[a].p || kstep_tasks[b].p)
    return false;

  if (!kstep_task_running(kstep_tasks[a].p))
      panic("Task %d is not on CPU when forking", a);
  kstep_task_fork(kstep_tasks[a].p, 1);
  p = find_new_child(kstep_tasks[a].p);
  if (!p)
    return false;

  kstep_tasks[b].p = p;
  kstep_tasks[b].cgroup_id = kstep_tasks[a].cgroup_id;
  return true;
}

static bool op_task_pin(int a, int b, int c) {
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when pinning", a);
  kstep_task_pin(kstep_tasks[a].p, b, c);
  return true;
}

static bool op_task_fifo(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;

  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting FIFO", a);
  // Move the task back to the root cgroup, otherwise the set_schedprio will fail
  move_task_to_root(a);
  kstep_task_fifo(kstep_tasks[a].p);
  return true;
}

static bool op_task_cfs(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting CFS", a);
  kstep_task_cfs(kstep_tasks[a].p);
  return true;
}

static bool op_task_pause(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when pausing", a);
  kstep_task_pause(kstep_tasks[a].p);
  return true;
}

static bool op_task_wakeup(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (kstep_tasks[a].p->__state == TASK_RUNNING)
    panic("Task %d is already on CPU when waking up", a);
  kstep_task_wakeup(kstep_tasks[a].p);
  return true;
}

static bool op_task_set_prio(int a, int b, int c) {
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (!kstep_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting priority", a);
  if (b < -20 || b > 19)
    return false;
  kstep_task_set_prio(kstep_tasks[a].p, b);
  return true;
}

static bool op_tick(int a, int b, int c) {
  (void)a;
  (void)b;
  (void)c;
  kstep_tick();
  return true;
}

static u64 count_ineligible_cgroup_se(void) {
  u64 count = 0;

  for (int id = 0; id < MAX_CGROUPS; id++) {
    struct task_group *tg;

    if (!cgroup_exists[id])
      continue;

    tg = cgroup_tg[id];
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

static bool op_tick_repeat(int a, int b, int c) {
  u8 executed_steps = 0;
  (void)b;
  (void)c;

  for (int i = 0; i < a; i++) {
    if (count_ineligible_cgroup_se() > 1)
      break;
    kstep_execute_op(OP_TICK, 0, 0, 0);
    executed_steps++;
    if (count_ineligible_cgroup_se() > 1)
      break;
  }

  last_executed_steps = executed_steps;
  return true;
}

static bool op_cgroup_create(int a, int b, int c) {
  int parent_id = a;
  int child_id = b;
  char name[MAX_CGROUP_NAME_LEN];
  (void)c;

  if (!is_valid_cgroup_id(child_id) || cgroup_exists[child_id])
    return false;
  if (parent_id != -1 && (!is_valid_cgroup_id(parent_id) || !cgroup_exists[parent_id]))
    return false;

  cgroup_parent_id[child_id] = parent_id;
  cgroup_exists[child_id] = true;

  if (!build_cgroup_name(child_id, name))
    return false;

  // Only leaf cgroups can contain tasks in cgroup v2.
  if (parent_id != -1)
    cgroup_move_tasks_to_root(parent_id);

  kstep_cgroup_create(name);
  cgroup_tg[child_id] = lookup_cgroup_task_group(name);
  if (!cgroup_tg[child_id])
    panic("Failed to resolve task group for cgroup %s", name);
  return true;
}

static bool op_cgroup_set_cpuset(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  char cpuset[32];

  if (!is_valid_cgroup_id(a) || !cgroup_exists[a])
    return false;
  if (!build_cgroup_name(a, name))
    return false;
  if (b > c || b < 1 || c > num_online_cpus() - 1)
    return false;

  if (scnprintf(cpuset, sizeof(cpuset), "%d-%d", b, c) >= sizeof(cpuset))
    return false;

  kstep_cgroup_set_cpuset(name, cpuset);
  return true;
}

static bool op_cgroup_set_weight(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  (void)c;

  if (!is_valid_cgroup_id(a) || !cgroup_exists[a])
    return false;
  if (!build_cgroup_name(a, name))
    return false;
  if (b <= 0 || b > 10000)
    return false;

  kstep_cgroup_set_weight(name, b);
  return true;
}

static bool op_cgroup_add_task(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  (void)c;

  if (!is_valid_cgroup_id(a) || !cgroup_exists[a])
    return false;
  if (!build_cgroup_name(a, name))
    return false;
  if (!is_valid_task_id(b) || !kstep_tasks[b].p)
    return false;

  if (kstep_tasks[b].p->policy != 0)
    kstep_task_cfs(kstep_tasks[b].p);
  
  kstep_cgroup_add_task(name, kstep_tasks[b].p->pid);

  kstep_tasks[b].cgroup_id = a;
  return true;
}

static bool op_cgroup_destroy(int a, int b, int c) {
  char name[MAX_CGROUP_NAME_LEN];
  (void)b;
  (void)c;

  if (!is_valid_cgroup_id(a) || !cgroup_exists[a])
    return false;
  if (!cgroup_is_leaf(a))
    return false;
  if (!build_cgroup_name(a, name))
    return false;

  cgroup_move_tasks_to_root(a);
  kstep_cgroup_destroy(name);
  cgroup_tg[a] = NULL;
  cgroup_exists[a] = false;
  cgroup_parent_id[a] = -1;
  return true;
}

static bool op_cgroup_move_task_root(int a, int b, int c) {
  (void)c;

  if (!is_valid_cgroup_id(a) || !cgroup_exists[a])
    return false;
  if (!is_valid_task_id(b) || !kstep_tasks[b].p)
    return false;
  if (kstep_tasks[b].cgroup_id != a)
    return false;

  move_task_to_root(b);
  return true;
}

typedef bool (*op_handler_fn)(int a, int b, int c);

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

#define for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)			\
	list_for_each_entry_safe(cfs_rq, pos, &rq->leaf_cfs_rq_list,	\
				 leaf_cfs_rq_list)

/* Checkers: work conserving, util_avg decay rate */
bool kstep_work_conserving_broken(void) {
  struct cpumask idle_cpus;
  int runnable_tasks = 0;
  bool eligible_runnable = false;

  cpumask_clear(&idle_cpus);
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);

    if (rq->nr_running < 0)
      panic("invalid nr_running value");
    if (rq->nr_running == 0)
      cpumask_set_cpu(cpu, &idle_cpus);
  }

  for (int i = 0; i < MAX_TASKS; i++) {
    struct task_struct *p = kstep_tasks[i].p;

    if (!p || p->__state != TASK_RUNNING)
      continue;

    runnable_tasks++;
    if (cpumask_intersects(p->cpus_ptr, &idle_cpus) && task_rq(p)->nr_running > 1) {
      eligible_runnable = true;
      TRACE_INFO("Runnable task %d (%d) can run on idle CPUs %*pbl",
                 i, p->pid, cpumask_pr_args(p->cpus_ptr));
    }
  }

  TRACE_INFO("work_conserving_check: runnable=%d idle_cpus=%*pbl eligible=%d",
             runnable_tasks, cpumask_pr_args(&idle_cpus), eligible_runnable);

  return runnable_tasks > num_online_cpus() - 1 && !cpumask_empty(&idle_cpus) && eligible_runnable;
}


static struct checker_result cr = {0};

struct checker_states {
  s64 cfs_util_avg[NR_CPUS];
  s64 rt_util_avg[NR_CPUS];
};

static struct checker_states checker_snapshot;

struct checker_result kstep_checker_result(void) {
  return (struct checker_result){
      .cfs_util_avg_decay = cr.cfs_util_avg_decay,
      .rt_util_avg_decay = cr.rt_util_avg_decay,
  };
}

static s64 get_cfs_util_avg(struct rq *rq) {
  s64 removed = 0;
  struct cfs_rq *cfs_rq, *pos;
  for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos) {
    removed += cfs_rq->removed.util_avg;
  }
  return rq->cfs.avg.util_avg - rq->cfs.removed.util_avg - removed;
}

static s64 get_rt_util_avg(struct rq *rq) {
  return rq->avg_rt.util_avg;
}

static void print_cgroup_state(int id) {
  char name[MAX_CGROUP_NAME_LEN];
  char eligible[256] = "";
  int len = 0;
  struct task_group *tg = cgroup_tg[id];

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

  for (int i = 0; i < MAX_CGROUPS; i++) {
    if (!cgroup_exists[i])
      continue;
    print_cgroup_state(i);
  }

  // for (int i = 1; i < num_online_cpus(); i++) {
  //   struct rq *rq = cpu_rq(i);
  //   pr_info("CPU %d: %lu, %lu, %llu, %llu, %lu\n", i, rq->cfs.avg.util_avg, rq->avg_rt.util_avg, rq->clock_task, rq->clock_pelt, rq->lost_idle_time);
  // }
}

/*
 * Try to send the head queued op for task @id if its required state is met.
 * Sends at most one op per call.
 */
static bool execute_one_op(enum kstep_op_type type, int a, int b, int c) {
  struct checker_states *cs = &checker_snapshot;
  memset(cs, 0, sizeof(*cs));
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    cs->cfs_util_avg[cpu] = get_cfs_util_avg(rq);
    cs->rt_util_avg[cpu] = get_rt_util_avg(rq);
  }

  kstep_cov_enable();
  pr_info("EXECOP: {\"op\": %d, \"a\": %d, \"b\": %d, \"c\": %d}\n", type, a, b, c);
  if (!op_handlers[type](a, b, c))
    // panic("Operation failed: %s %d %d %d\n", op_strs[type], a, b, c);
    return false;
  kstep_cov_disable();
  kstep_cov_dump();
  kstep_cov_cmd_id_inc();
  print_state();

  for (int i = 0; i < MAX_TASKS; i++) {
    struct task_struct *p = kstep_tasks[i].p;
    if (!p)
      continue;
    // Don't force decay slowly for task migrating cpus or classes
    if (kstep_tasks[i].cur_cpu != task_cpu(p)) {
      cs->cfs_util_avg[kstep_tasks[i].cur_cpu] = 0;
      kstep_tasks[i].cur_cpu = task_cpu(p);
    }
    if (kstep_tasks[i].cur_policy != p->policy) {
      cs->cfs_util_avg[kstep_tasks[i].cur_cpu] = 0;
      kstep_tasks[i].cur_policy = p->policy;
    }
  }

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);

    if (cs->cfs_util_avg[cpu] - get_cfs_util_avg(rq) > 1010) {
      cr.cfs_util_avg_decay += 1;
      TRACE_INFO("cfs_util_avg_decay broken on cpu %d", cpu);
    }

    else if (cs->rt_util_avg[cpu] - get_rt_util_avg(rq) > 1010) {
      cr.rt_util_avg_decay += 1;
      TRACE_INFO("rt_util_avg_decay broken on cpu %d", cpu);
    }

    else if (cs->rt_util_avg[cpu] + cs->cfs_util_avg[cpu] - get_cfs_util_avg(rq) - get_rt_util_avg(rq) > 1010) {
      cr.rt_util_avg_decay += 1;
      TRACE_INFO("total cfs & rt_util_avg_decay broken on cpu %d", cpu);
    }
  }

  return true;
}

static u8 buf[1 + 1 + 1 + MAX_TASKS * 2 + 1];

u8 kstep_last_executed_steps(void) {
  return last_executed_steps;
}

static u8 encode_state_byte(u8 val) {
  return val + 11; /* avoid '\n' in the binary frame */
}

void kstep_write_state(struct file *f, bool executed, u8 executed_steps) {
  /* Format: [OP_TYPE_NR] [executed] [executed_steps+11] [id] [state] ... ['\n']
   * OP_TYPE_NR is the marker byte; executed and state are one byte, and
   * executed_steps is offset to avoid '\n'. 2 = running on CPU,
   * 1 = runnable on runqueue, 0 = blocked. */
  loff_t pos = 0;
  int len = 0;
  

  buf[len++] = OP_TYPE_NR;
  buf[len++] = (u8) executed;
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
  buf[len++] = '\n';
  kernel_write(f, buf, len, &pos);
}

bool kstep_execute_op(enum kstep_op_type type, int a, int b, int c) {
  last_executed_steps = 0;
  if (type < 0 || type >= OP_TYPE_NR)
    panic("Operation failed: %d %d %d %d\n", type, a, b, c);
  if (!op_handlers[type]) {
    TRACE_INFO("Operation not implemented: %d\n", type);
    return false;
  }

  if (type == OP_TICK_REPEAT) {
    op_handlers[type](a, b, c);
    return true;
  }

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
      return false;
    }
  }

  if (!execute_one_op(type, a, b, c))
    return false;

  last_executed_steps = 1;
  return true;
}
