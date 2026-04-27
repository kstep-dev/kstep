#include <linux/kernel.h>
#include <linux/sched/signal.h>

#include "op_handler_internal.h"

static bool pid_known(pid_t pid) {
  for (int i = 0; i < MAX_TASKS; i++) {
    if (kstep_tasks[i].p && kstep_tasks[i].p->pid == pid)
      return true;
  }
  return false;
}

bool kstep_op_task_running(struct task_struct *p) {
#ifdef TIF_NEED_RESCHED_LAZY
  return p->on_cpu && !test_tsk_thread_flag(p, TIF_NEED_RESCHED_LAZY) &&
         !test_tsk_thread_flag(p, TIF_NEED_RESCHED);
#else
  return p->on_cpu && !test_tsk_thread_flag(p, TIF_NEED_RESCHED);
#endif
}

static struct task_struct *find_new_child(struct task_struct *parent) {
  struct task_struct *p;

  for (int attempt = 0; attempt < 100; attempt++) {
    for_each_process(p) {
      if ((p->real_parent == parent || p->parent == parent) &&
          p->pid > parent->pid && !pid_known(p->pid))
        return p;
    }
    kstep_sleep();
  }

  panic("No new child found for parent %d", parent->pid);
}

u8 kstep_op_task_create(int a, int b, int c) {
  (void)b;
  (void)c;

  if (!kstep_op_is_valid_task_id(a) || kstep_tasks[a].p)
    panic("Invalid task id");
  kstep_tasks[a].p = kstep_task_create();
  kstep_tasks[a].cgroup_id = -1;
  return 1;
}

u8 kstep_op_task_fork(int a, int b, int c) {
  struct task_struct *p;

  (void)c;
  if (!kstep_op_is_valid_task_id(a) || !kstep_op_is_valid_task_id(b))
    return 0;
  if (!kstep_tasks[a].p || kstep_tasks[b].p)
    return 0;

  if (!kstep_op_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when forking", a);
  kstep_task_fork(kstep_tasks[a].p, 1);
  p = find_new_child(kstep_tasks[a].p);
  if (!p)
    return 0;

  kstep_tasks[b].p = p;
  kstep_tasks[b].cgroup_id = kstep_tasks[a].cgroup_id;
  return 1;
}

u8 kstep_op_task_pin(int a, int b, int c) {
  if (!kstep_op_is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_op_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when pinning", a);
  kstep_task_pin(kstep_tasks[a].p, b, c);
  return 1;
}

u8 kstep_op_task_fifo(int a, int b, int c) {
  (void)b;
  (void)c;

  if (!kstep_op_is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_op_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting FIFO", a);

  /* Move the task back to the root cgroup, otherwise set_schedprio will fail. */
  kstep_op_move_task_to_root(a);
  kstep_task_fifo(kstep_tasks[a].p);
  return 1;
}

u8 kstep_op_task_cfs(int a, int b, int c) {
  (void)b;
  (void)c;

  if (!kstep_op_is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_op_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting CFS", a);
  kstep_task_cfs(kstep_tasks[a].p);
  return 1;
}

u8 kstep_op_task_pause(int a, int b, int c) {
  (void)b;
  (void)c;

  if (!kstep_op_is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_op_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when pausing", a);
  kstep_task_pause(kstep_tasks[a].p);
  return 1;
}

u8 kstep_op_task_wakeup(int a, int b, int c) {
  struct task_struct *p;

  (void)b;
  (void)c;

  if (!kstep_op_is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  p = kstep_tasks[a].p;
  if (p->__state == TASK_RUNNING)
    panic("Task %d is already on CPU when waking up", a);
  if (READ_ONCE(p->__state) & TASK_FROZEN)
    kstep_thaw_task(p);
  kstep_task_wakeup(p);
  return 1;
}

u8 kstep_op_task_freeze(int a, int b, int c) {
  (void)b;
  (void)c;

  if (!kstep_op_is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (kstep_tasks[a].p->__state == TASK_RUNNING)
    panic("Task %d is running when freezing", a);
  kstep_freeze_task(kstep_tasks[a].p);
  return 1;
}

u8 kstep_op_task_set_prio(int a, int b, int c) {
  (void)c;

  if (!kstep_op_is_valid_task_id(a) || !kstep_tasks[a].p)
    return 0;
  if (!kstep_op_task_running(kstep_tasks[a].p))
    panic("Task %d is not on CPU when setting priority", a);
  if (b < -20 || b > 19)
    return 0;
  kstep_task_set_prio(kstep_tasks[a].p, b);
  return 1;
}
