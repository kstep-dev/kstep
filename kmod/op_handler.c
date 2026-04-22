#include <linux/kernel.h>

#include "checker.h"
#include "op_handler_internal.h"
#include "linux/sched.h"

struct kstep_task kstep_tasks[MAX_TASKS];
struct kstep_managed_kthread kstep_kthreads[KSTEP_MAX_KTHREADS];
struct kstep_cgroup_state kstep_cgroups[MAX_CGROUPS];
int cgroup_lineage[MAX_CGROUPS];

bool kstep_op_is_valid_task_id(int id) { return id >= 0 && id < MAX_TASKS; }
bool kstep_op_is_valid_cgroup_id(int id) { return id >= 0 && id < MAX_CGROUPS; }
bool kstep_op_is_valid_kthread_id(int id) {
  return id >= 0 && id < KSTEP_MAX_KTHREADS;
}

bool kstep_build_cgroup_name(int id, char *buf) {
  int depth = 0;
  int cur = id;
  int len = 0;

  while (cur != -1) {
    if (!kstep_op_is_valid_cgroup_id(cur) || !kstep_cgroups[cur].exists ||
        depth >= MAX_CGROUPS)
      return false;
    cgroup_lineage[depth++] = cur;
    cur = kstep_cgroups[cur].parent_id;
  }

  for (int i = depth - 1; i >= 0; i--) {
    len += scnprintf(buf + len, MAX_CGROUP_NAME_LEN - len, "cg%d%s",
                     cgroup_lineage[i], (i > 0) ? "/" : "");
    if (len >= MAX_CGROUP_NAME_LEN)
      return false;
  }

  return true;
}

typedef u8 (*op_handler_fn)(int a, int b, int c);

static op_handler_fn op_handlers[OP_TYPE_NR] = {
    [OP_TASK_CREATE] = kstep_op_task_create,
    [OP_TASK_FORK] = kstep_op_task_fork,
    [OP_TASK_PIN] = kstep_op_task_pin,
    [OP_TASK_FIFO] = kstep_op_task_fifo,
    [OP_TASK_CFS] = kstep_op_task_cfs,
    [OP_TASK_PAUSE] = kstep_op_task_pause,
    [OP_TASK_WAKEUP] = kstep_op_task_wakeup,
    [OP_TASK_SET_PRIO] = kstep_op_task_set_prio,
    [OP_TICK] = kstep_op_tick,
    [OP_TICK_REPEAT] = kstep_op_tick_repeat,
    [OP_CGROUP_CREATE] = kstep_op_cgroup_create,
    [OP_CGROUP_SET_CPUSET] = kstep_op_cgroup_set_cpuset,
    [OP_CGROUP_SET_WEIGHT] = kstep_op_cgroup_set_weight,
    [OP_CGROUP_ADD_TASK] = kstep_op_cgroup_add_task,
    [OP_CPU_SET_FREQ] = NULL,
    [OP_CPU_SET_CAPACITY] = NULL,
    [OP_CGROUP_DESTROY] = kstep_op_cgroup_destroy,
    [OP_CGROUP_MOVE_TASK_ROOT] = kstep_op_cgroup_move_task_root,
    [OP_KTHREAD_CREATE] = kstep_op_kthread_create,
    [OP_KTHREAD_BIND] = kstep_op_kthread_bind,
    [OP_KTHREAD_START] = kstep_op_kthread_start,
    [OP_KTHREAD_YIELD] = kstep_op_kthread_yield,
    [OP_KTHREAD_BLOCK] = kstep_op_kthread_block,
    [OP_KTHREAD_SYNCWAKE] = kstep_op_kthread_syncwake,
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
    return p->__state != TASK_RUNNING; /* task must be blocked/dequeued */
  return kstep_op_task_running(p);     /* all other signal ops need on-cpu */
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

  if (!kstep_build_cgroup_name(id, name))
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
    pr_info("Task %d %d: on_cpu=%d, state=%d, cpu=%d, class=%d, util_avg=%lu, eligible=%d\n",
            i, p->pid, kstep_op_task_running(p), p->__state, task_cpu(p),
            p->policy, p->se.avg.util_avg, kstep_eligible(&p->se));
  }

  for (int i = 0; i < KSTEP_MAX_KTHREADS; i++) {
    enum kstep_kthread_state state;

    if (!kstep_kthreads[i].p)
      continue;
    state = kstep_op_kthread_state(i);
    pr_info("Kthread %d: pid=%d, cpu=%d, state=%d\n", i,
            state == KSTEP_KTHREAD_DEAD ? -1 : kstep_kthreads[i].p->pid,
            task_cpu(kstep_kthreads[i].p), kstep_kthreads[i].p->__state);
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
  kstep_check_after_op(&check_state, type, a, b, c);

  return executed_steps;
}

static u8 buf[1 + 1 + 1 + MAX_TASKS * 2 + 1 + KSTEP_MAX_KTHREADS * 2 + 1];

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
  buf[len++] = executed_steps + 11;
  for (int i = 0; i < MAX_TASKS; i++) {
    struct task_struct *p = kstep_tasks[i].p;
    u8 state;

    if (!p)
      continue;
    if (kstep_op_task_running(p))
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
    state = kstep_op_kthread_state(i);
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
    if (!kstep_op_is_valid_task_id(a) || !kstep_tasks[a].p)
      panic("Task %d not found", a);
    if (!op_task_state_ready(type, kstep_tasks[a].p)) {
      // directly return when task is not in the required state
      // then, the executor will send the latest state to the input generator
      TRACE_INFO("Task %d is not in the required state for operation %s", a,
                 op_strs[type]);
      return 0;
    }
  }

  return execute_one_op(type, a, b, c);
}
