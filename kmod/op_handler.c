#include <linux/kernel.h> // panic, scnprintf
#include <linux/sched/signal.h> // for_each_process
#include <linux/string.h> // strcmp, strscpy

#include "driver.h"
#include "op_handler.h"
#include "user.h"

#define MAX_TASKS 1024

static struct task_struct *kstep_tasks[MAX_TASKS];

static bool is_valid_task_id(int id) { return id >= 0 && id < MAX_TASKS; }

static bool pid_known(pid_t pid) {
  for (int i = 0; i < MAX_TASKS; i++) {
    if (kstep_tasks[i] && kstep_tasks[i]->pid == pid)
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
  return NULL;
}

static bool op_task_create(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || kstep_tasks[a])
    panic("Invalid task id");
  kstep_tasks[a] = kstep_task_create();
  return true;
}

static struct task_struct *waker_task;
static void *waker_on_cpu(void) {
  if (waker_task && waker_task->on_cpu)
    return waker_task;
  return NULL;
}

static bool op_task_fork(int a, int b, int c) {
  struct task_struct *p;
  (void)c;
  
  if (!is_valid_task_id(a) || !kstep_tasks[a])
    panic("Invalid waker task id");
  if (!is_valid_task_id(b) || kstep_tasks[b])
    panic("Invalid wakee task id");

  waker_task = kstep_tasks[a];
  kstep_tick_until(waker_on_cpu);
  
  kstep_task_fork(waker_task, 1);
  p = find_new_child(kstep_tasks[a]);
  if (!p)
    panic("Failed to find forked child");

  kstep_tasks[b] = p;
  return true;
}

static bool op_task_pin(int a, int b, int c) {
  if (!is_valid_task_id(a) || !kstep_tasks[a])
    panic("Invalid task id");
  kstep_task_pin(kstep_tasks[a], b, c);
  return true;
}

static bool op_task_fifo(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a])
    panic("Invalid task id");
  kstep_task_fifo(kstep_tasks[a]);
  return true;
}

static bool op_task_pause(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a])
    panic("Invalid task id");
  kstep_task_pause(kstep_tasks[a]);
  return true;
}

static bool op_task_wakeup(int a, int b, int c) {
  (void)b;
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a])
    panic("Invalid task id");
  kstep_task_wakeup(kstep_tasks[a]);
  return true;
}

static bool op_task_set_prio(int a, int b, int c) {
  (void)c;
  if (!is_valid_task_id(a) || !kstep_tasks[a])
    panic("Invalid task id");
  kstep_task_set_prio(kstep_tasks[a], b);
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
  kstep_tick_repeat(a);
  return true;
}

typedef bool (*op_handler_fn)(int a, int b, int c);

static op_handler_fn op_handlers[OP_TYPE_NR] = {
    [OP_TASK_CREATE] = op_task_create,
    [OP_TASK_FORK] = op_task_fork,
    [OP_TASK_PIN] = op_task_pin,
    [OP_TASK_FIFO] = op_task_fifo,
    [OP_TASK_PAUSE] = op_task_pause,
    [OP_TASK_WAKEUP] = op_task_wakeup,
    [OP_TASK_SET_PRIO] = op_task_set_prio,
    [OP_TICK] = op_tick,
    [OP_TICK_REPEAT] = op_tick_repeat,
    [OP_CGROUP_CREATE] = NULL,
    [OP_CGROUP_SET_CPUSET] = NULL,
    [OP_CGROUP_SET_WEIGHT] = NULL,
    [OP_CGROUP_ADD_TASK] = NULL,
    [OP_CPU_SET_FREQ] = NULL,
    [OP_CPU_SET_CAPACITY] = NULL,
};

bool kstep_execute_op(enum kstep_op_type type, int a, int b, int c) {
  if (type < 0 || type >= OP_TYPE_NR)
    return false;
  if (!op_handlers[type]) {
    TRACE_INFO("Operation not implemented: %d\n", type);
    return false;
  }
  return op_handlers[type](a, b, c);
}
