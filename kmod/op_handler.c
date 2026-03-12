#include <linux/kernel.h> // panic, scnprintf
#include <linux/sched/signal.h> // for_each_process
#include <linux/string.h> // strcmp, strscpy

#include "driver.h"
#include "internal.h"
#include "op_handler.h"
#include "user.h"

#define MAX_TASKS 1024
#define MAX_CGROUPS 1024
#define MAX_CGROUP_NAME_LEN 256

static int cgroup_parent_id[MAX_CGROUPS];
static bool cgroup_exists[MAX_CGROUPS];
static int cgroup_lineage[MAX_CGROUPS];

#define TASK_OP_QUEUE_SIZE 64

struct queued_op { enum kstep_op_type type; int a, b, c; u64 seq; };

struct kstep_task {
  struct task_struct *p;
  int cgroup_id;
  struct queued_op ring[TASK_OP_QUEUE_SIZE];
  int head, tail;
};

static struct kstep_task kstep_tasks[MAX_TASKS];

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

static bool pid_known(pid_t pid) {
  for (int i = 0; i < MAX_TASKS; i++) {
    if (kstep_tasks[i].p && kstep_tasks[i].p->pid == pid)
      return true;
  }
  return false;
}

static struct task_struct *find_new_child(struct task_struct *parent) {
  struct task_struct *p;
  for (int attempt = 0; attempt < 100; attempt++) {
    for_each_process(p) {
      if ((p->real_parent == parent || p->parent == parent) &&
          strcmp(p->comm, TASK_READY_COMM) == 0 && !pid_known(p->pid))
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

  if (kstep_tasks[a].p->on_cpu == false)
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
  if (kstep_tasks[a].p->on_cpu == false)
    panic("Task %d is not on CPU when pinning", a);
  kstep_task_pin(kstep_tasks[a].p, b, c);
  return true;
}

static bool op_task_fifo(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;

  if (kstep_tasks[a].p->on_cpu == false)
    panic("Task %d is not on CPU when setting FIFO", a);
  // Move the task back to the root cgroup, otherwise the set_schedprio will fail
  kstep_cgroup_add_task("", kstep_tasks[a].p->pid);
  kstep_tasks[a].cgroup_id = -1;
  kstep_task_fifo(kstep_tasks[a].p);
  return true;
}

static bool op_task_cfs(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (kstep_tasks[a].p->on_cpu == false)
    panic("Task %d is not on CPU when setting CFS", a);
  kstep_task_cfs(kstep_tasks[a].p);
  return true;
}

static bool op_task_pause(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (kstep_tasks[a].p->on_cpu == false)
    panic("Task %d is not on CPU when pausing", a);
  kstep_task_pause(kstep_tasks[a].p);
  return true;
}

static bool op_task_wakeup(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (kstep_tasks[a].p->on_cpu == true)
    panic("Task %d is already on CPU when waking up", a);
  kstep_task_wakeup(kstep_tasks[a].p);
  return true;
}

static bool op_task_set_prio(int a, int b, int c) {
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a].p)
    return false;
  if (kstep_tasks[a].p->on_cpu == false)
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

static bool op_tick_repeat(int a, int b, int c) {
  (void)b;
  (void)c;
  for (int i = 0; i < a; i++)
    kstep_execute_op(OP_TICK, 0, 0, 0);
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

  // Move the task back to the root cgroup: only the leaf cgroup has tasks in cgroupv2
  for (int i = 0; i < MAX_TASKS; i++) {
    if (parent_id != -1 && kstep_tasks[i].p && kstep_tasks[i].cgroup_id == parent_id) {
      kstep_cgroup_add_task("", kstep_tasks[i].p->pid);
      kstep_tasks[i].cgroup_id = -1;
    }
  }

  kstep_cgroup_create(name);
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

  kstep_cgroup_add_task(name, kstep_tasks[b].p->pid);

  kstep_tasks[b].cgroup_id = a;
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
};


/* Returns true if task p is in the state required to receive op type. */
static bool op_task_state_ready(enum kstep_op_type type, struct task_struct *p) {
  if (type == OP_TASK_WAKEUP)
    return !task_is_runnable(p);           /* task must be blocked/dequeued */
  return p->on_cpu;             /* all other signal ops need on-cpu */
}

static bool is_task_signal_op(enum kstep_op_type type) {
  switch (type) {
  case OP_TASK_FORK: case OP_TASK_PIN:  case OP_TASK_FIFO:
  case OP_TASK_CFS:  case OP_TASK_PAUSE: case OP_TASK_WAKEUP:
  case OP_TASK_SET_PRIO: return true;
  default: return false;
  }
}

/*
 * Enqueue a signal op for task @id.  Must only be called when the queue
 * is non-empty OR the task is not in the required state.
 */
static u64 op_seq_counter;

static void enqueue_op(int task_id, enum kstep_op_type type, int a, int b, int c) {
  struct kstep_task *t = &kstep_tasks[task_id];
  int next = (t->tail + 1) % TASK_OP_QUEUE_SIZE;
  if (next == t->head)
    panic("task op queue overflow for task %d", task_id);
  t->ring[t->tail] = (struct queued_op){type, a, b, c, op_seq_counter++};
  t->tail = next;
}


static void task_queues_drain(void);

/*
 * Try to send the head queued op for task @id if its required state is met.
 * Sends at most one op per call.
 */
static void execute_one_op(enum kstep_op_type type, int a, int b, int c) {
  kstep_cov_enable();
  pr_info("EXECOP: {\"op\": %d, \"a\": %d, \"b\": %d, \"c\": %d}\n", type, a, b, c);
  if (!op_handlers[type](a, b, c))
    panic("Operation failed: %s %d %d %d\n", op_strs[type], a, b, c);
  kstep_cov_disable();
  kstep_cov_dump();
  kstep_cov_cmd_id_inc();
  task_queues_drain();
}

static void task_queues_drain(void) {
  int best = -1;
  u64 best_seq = U64_MAX;

  TRACE_INFO("Draining task queues");

  for (int i = 0; i < MAX_TASKS; i++) {
    struct kstep_task *t = &kstep_tasks[i];
    if (!t->p || t->head == t->tail) continue;
    if (!op_task_state_ready(t->ring[t->head].type, t->p)) continue;
    if (t->ring[t->head].seq < best_seq) {
      best_seq = t->ring[t->head].seq;
      best = i;
    }
  }

  if (best == -1) return;

  struct kstep_task *t = &kstep_tasks[best];
  struct queued_op op = t->ring[t->head];
  t->head = (t->head + 1) % TASK_OP_QUEUE_SIZE;
  execute_one_op(op.type, op.a, op.b, op.c);
}

void kstep_execute_op(enum kstep_op_type type, int a, int b, int c) {
  if (type < 0 || type >= OP_TYPE_NR)
    panic("Operation failed: %d %d %d %d\n", type, a, b, c);
  if (!op_handlers[type]) {
    TRACE_INFO("Operation not implemented: %d\n", type);
    return;
  }

  if (type == OP_TICK_REPEAT) {
    op_handlers[type](a, b, c);
    return;
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
    struct kstep_task *t = &kstep_tasks[a];
    bool queue_busy = (t->head != t->tail);
    if (queue_busy || !op_task_state_ready(type, t->p)) {
      enqueue_op(a, type, a, b, c);
      return;
    }
  }

  execute_one_op(type, a, b, c);
}
