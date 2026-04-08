#ifndef KSTEP_DRIVER_H
#define KSTEP_DRIVER_H

#include "invariant.h"
#include <linux/sched.h>

#define TRACE_INFO(fmt, ...) pr_info("\033[92m" fmt "\033[0m\n", ##__VA_ARGS__)
#define DRIVER_NAME_LEN 32    

struct sched_domain;
struct kstep_driver {
  char name[DRIVER_NAME_LEN];
  void (*setup)(void);
  void (*run)(void);
  // Callbacks before and after each tick
  void (*on_tick_begin)(void);
  void (*on_tick_end)(void);
  // Callbacks before and after load balancing softirq
  void (*on_sched_softirq_begin)(void);
  void (*on_sched_softirq_end)(void);
  // Callback at sched_balance_rq
  void (*on_sched_balance_begin)(int cpu, struct sched_domain *sd);
  // Callback after should_we_balance
  void (*on_sched_balance_selected)(int cpu, struct sched_domain *sd);
  // Callback at init_tg_cfs_entry (new task group cfs_rq created)
  void (*on_sched_group_alloc)(struct task_group *tg, int cpu);
  u64 step_interval_us;                // Real time sleep between steps in us
  u64 tick_interval_ns;                // Virtual clock advance per tick in ns
  struct kstep_invariant **invariants; // NULL-terminated array of invariants
};
#define KSTEP_DRIVER_DEFINE static struct kstep_driver DRIVER __used =

// output.c
struct kstep_json {
  size_t len;
  char buf[512 - sizeof(size_t)];
};
void kstep_json_begin(struct kstep_json *json);
void kstep_json_field_fmt(struct kstep_json *json, const char *key,
                          const char *val_fmt, ...);
void kstep_json_field_str(struct kstep_json *json, const char *key,
                          const char *val);
void kstep_json_field_u64(struct kstep_json *json, const char *key, u64 val);
void kstep_json_field_s64(struct kstep_json *json, const char *key, s64 val);
void kstep_json_end(struct kstep_json *json);
void kstep_json_print_2kv(const char *key1, const char *val1, const char *key2,
                          const char *val2_fmt, ...);
#define kstep_pass(msg_fmt, ...)                                               \
  kstep_json_print_2kv("status", "pass", "message", "\"" msg_fmt "\"",         \
                       ##__VA_ARGS__)
#define kstep_fail(msg_fmt, ...)                                               \
  kstep_json_print_2kv("status", "fail", "message", "\"" msg_fmt "\"",         \
                       ##__VA_ARGS__)
void kstep_print_sched_debug(void);
void kstep_output_curr_task(void);
void kstep_output_nr_running(void);
void kstep_output_balance(int cpu, struct sched_domain *sd);

// tick.c
void kstep_tick(void);
void kstep_tick_repeat(int n);
void *kstep_tick_until(void *(*fn)(void));
void kstep_sleep(void);
void *kstep_sleep_until(void *(*fn)(void));

// task.c
struct task_struct *kstep_task_create(void);
void kstep_task_pin(struct task_struct *p, int begin, int end);
void kstep_task_fork(struct task_struct *p, int n);
void kstep_task_fifo(struct task_struct *p);
void kstep_task_cfs(struct task_struct *p);
void kstep_task_pause(struct task_struct *p);
void kstep_task_wakeup(struct task_struct *p);
void kstep_task_block(struct task_struct *p);
void kstep_task_set_prio(struct task_struct *p, int prio);
void kstep_task_kernel_pause(struct task_struct *p);
void kstep_task_kernel_wakeup(struct task_struct *p);

// kernel.c
void kstep_write(const char *path, const char *buf, size_t size);
void kstep_mkdir(const char *dir);
void kstep_sysctl_write(const char *name, const char *fmt, ...);
void kstep_sched_feat_write(const char *fmt, ...);
void kstep_sched_feat_enable(const char *name);
void kstep_sched_feat_disable(const char *name);
void kstep_cgroup_write(const char *name, const char *filename, const char *fmt,
                        ...);
void kstep_cgroup_create(const char *name);
void kstep_cgroup_destroy(const char *name);
void kstep_cgroup_set_cpuset(const char *name, const char *cpuset);
void kstep_cgroup_set_weight(const char *name, int weight);
void kstep_cgroup_add_task(const char *name, int pid);
void kstep_freeze_task(struct task_struct *p);
int kstep_eligible(struct sched_entity *se);

// kthread.c
struct task_struct *kstep_kthread_create(const char *name);
void kstep_kthread_bind(struct task_struct *p, const struct cpumask *mask);
void kstep_kthread_start(struct task_struct *p);
void kstep_kthread_syncwake(struct task_struct *waker, struct task_struct *wakee);
void kstep_kthread_block(struct task_struct *p);
void kstep_kthread_yield(struct task_struct *p);

// topo.c
void kstep_topo_init(void);
void kstep_topo_set_smt(const char *cpulists[], int size);
void kstep_topo_set_cls(const char *cpulists[], int size);
void kstep_topo_set_mc(const char *cpulists[], int size);
void kstep_topo_set_pkg(const char *cpulists[], int size);
void kstep_topo_set_node(const char *cpulists[], int size);
void kstep_topo_apply(void);
void kstep_topo_param_apply(void);
void kstep_freq_param_apply(void);
void kstep_topo_print(void);
void kstep_cpu_set_freq(int cpu, int scale);
void kstep_cpu_set_capacity(int cpu, int scale);

#endif
