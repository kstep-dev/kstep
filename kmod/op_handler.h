#ifndef KSTEP_OP_HANDLER_H
#define KSTEP_OP_HANDLER_H

#include <linux/fs.h>
#include <linux/types.h>
#include "internal.h"

enum kstep_op_type {
  OP_TASK_CREATE,
  OP_TASK_FORK,
  OP_TASK_PIN,
  OP_TASK_FIFO,
  OP_TASK_CFS,
  OP_TASK_PAUSE,
  OP_TASK_WAKEUP,
  OP_TASK_SET_PRIO,
  OP_TICK,
  OP_TICK_REPEAT,
  OP_CGROUP_CREATE,
  OP_CGROUP_SET_CPUSET,
  OP_CGROUP_SET_WEIGHT,
  OP_CGROUP_ADD_TASK,
  OP_CPU_SET_FREQ,
  OP_CPU_SET_CAPACITY,
  OP_CGROUP_DESTROY,
  OP_CGROUP_MOVE_TASK_ROOT,
  OP_KTHREAD_CREATE,
  OP_KTHREAD_BIND,
  OP_KTHREAD_START,
  OP_KTHREAD_YIELD,
  OP_KTHREAD_BLOCK,
  OP_KTHREAD_SYNCWAKE,
  OP_TYPE_NR,
};

struct checker_result {
  int cfs_util_avg_decay;
  int rt_util_avg_decay;
};

bool kstep_execute_op(enum kstep_op_type type, int a, int b, int c);
u8 kstep_last_executed_steps(void);
void kstep_write_state(struct file *f, bool executed, u8 executed_steps);
bool kstep_work_conserving_broken(void);
struct checker_result kstep_checker_result(void);
void kstep_check_extra_balance(int cpu, struct sched_domain *sd);

#endif
