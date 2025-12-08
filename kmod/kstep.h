#ifndef KSTEP_H
#define KSTEP_H

#include <linux/cpumask.h>
#include <linux/types.h>
#include <linux/version.h>

// private headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#include <kernel/sched/sched.h>
#include <kernel/time/tick-sched.h>
#pragma GCC diagnostic pop

#include "logging.h"

#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

// main.c
struct kstep_params_t {
  char driver[32];                     // Name of the driver to run
  unsigned long long step_interval_us; // Interval between steps in us
  bool special_topo;                   // Whether to use the special topology
  bool print_rq_stats;                 // Whether to print rq stats
  bool print_tasks;                    // Whether to print tasks
  bool print_nr_running;               // Whether to print nr_running
  bool print_lb_events;                // Whether to print LB events
};
extern struct kstep_params_t kstep_params;
void kstep_params_print(void);

// driver.c
struct kstep_driver {
  const char *name;
  void (*pre_init)(void);
  void (*init)(void);
  void (*body)(void);
};
struct kstep_driver *kstep_driver_get(const char *name);

// tick.c
void kstep_tick_init(void);
void kstep_tick_exit(void);
void kstep_sleep(void);
void kstep_tick(void);
void kstep_tick_repeat(int n);
// Call tick until the function returns something, then return that something
void *kstep_tick_until(void *(*fn)(void));

// tasks.c
struct task_struct *kstep_task_create(void);
void kstep_task_pin(struct task_struct *p, int begin, int end);
void kstep_task_fork(struct task_struct *p, int n);
void kstep_task_fork_pin(struct task_struct *p, int n, int begin, int end);
void kstep_task_fifo(struct task_struct *p);
void kstep_task_pause(struct task_struct *p);
void kstep_task_wakeup(struct task_struct *p);
void kstep_task_sleep(struct task_struct *p, int n);
void kstep_task_set_prio(struct task_struct *p, int prio);
// Low level signal sending, use with caution, see "user.h"
enum sigcode;
void kstep_task_signal(struct task_struct *p, enum sigcode code, int val1,
                       int val2, int val3);
int is_sys_kthread(struct task_struct *p);

// kernel.c
void kstep_write(const char *path, const char *buf, size_t size);
void kstep_mkdir(const char *dir);
void kstep_cgroup_write(const char *name, const char *filename, const char *fmt,
                        ...);
void kstep_cgroup_create_pinned(const char *name, const char *cpuset);
void kstep_cgroup_set_weight(const char *name, int weight);
void kstep_cgroup_add_task(const char *name, int pid);
void kstep_freeze_task(struct task_struct *p);
int kstep_eligible(struct sched_entity *se);

// output.c
void print_rq_stats(void);
void print_tasks(void);
void print_nr_running(void);
void print_all_tasks(void);

// reset.c
void kstep_reset_sched(void);
void kstep_reset_task(struct task_struct *p);

// isolation.c
void kstep_disable_workqueue(void);
void kstep_move_kthreads(void);
void kstep_prealloc_kworkers(void);

// trace.c
void kstep_trace_lb(void);
void kstep_trace_rebalance(void);
void kstep_patch_min_vruntime(void);

// topo.c
void kstep_topo_print(void);
void kstep_use_special_topo(void);
void kstep_set_cpu_freq(int cpu, int scale);
void kstep_set_cpu_capacity(int cpu, int scale);

// ksym.c
struct ksym_t {
#define KSYM_FUNC(ret_type, name, ...) ret_type (*name)(__VA_ARGS__);
#define KSYM_VAR(type, name) type *name;
#include "ksym.h"
#undef KSYM_FUNC
#undef KSYM_VAR
};
extern struct ksym_t ksym;
void ksym_init(void);

#undef cpu_rq
#define cpu_rq(cpu) (per_cpu_ptr(ksym.runqueues, (cpu)))
#undef this_rq
#define this_rq() this_cpu_ptr(ksym.runqueues)
#undef raw_rq
#define raw_rq() raw_cpu_ptr(ksym.runqueues)

#endif
