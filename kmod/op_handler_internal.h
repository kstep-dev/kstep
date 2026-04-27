#ifndef KSTEP_OP_HANDLER_INTERNAL_H
#define KSTEP_OP_HANDLER_INTERNAL_H

#include "op_handler.h"
#include "op_state.h"

bool kstep_op_is_valid_task_id(int id);
bool kstep_op_is_valid_cgroup_id(int id);
bool kstep_op_is_valid_kthread_id(int id);

bool kstep_op_task_running(struct task_struct *p);
enum kstep_kthread_state kstep_op_kthread_state(int id);

bool kstep_op_cgroup_is_leaf(int id);
struct task_group *kstep_op_lookup_cgroup_task_group(const char *name);
void kstep_op_cgroup_move_tasks_to_root(int id);
void kstep_op_move_task_to_root(int task_id);

u8 kstep_op_task_create(int a, int b, int c);
u8 kstep_op_task_fork(int a, int b, int c);
u8 kstep_op_task_pin(int a, int b, int c);
u8 kstep_op_task_fifo(int a, int b, int c);
u8 kstep_op_task_cfs(int a, int b, int c);
u8 kstep_op_task_pause(int a, int b, int c);
u8 kstep_op_task_wakeup(int a, int b, int c);
u8 kstep_op_task_freeze(int a, int b, int c);
u8 kstep_op_task_set_prio(int a, int b, int c);

u8 kstep_op_tick(int a, int b, int c);
u8 kstep_op_tick_repeat(int a, int b, int c);

u8 kstep_op_cgroup_create(int a, int b, int c);
u8 kstep_op_cgroup_set_cpuset(int a, int b, int c);
u8 kstep_op_cgroup_set_weight(int a, int b, int c);
u8 kstep_op_cgroup_add_task(int a, int b, int c);
u8 kstep_op_cgroup_destroy(int a, int b, int c);
u8 kstep_op_cgroup_move_task_root(int a, int b, int c);

u8 kstep_op_kthread_create(int a, int b, int c);
u8 kstep_op_kthread_bind(int a, int b, int c);
u8 kstep_op_kthread_start(int a, int b, int c);
u8 kstep_op_kthread_yield(int a, int b, int c);
u8 kstep_op_kthread_block(int a, int b, int c);
u8 kstep_op_kthread_syncwake(int a, int b, int c);

#endif
