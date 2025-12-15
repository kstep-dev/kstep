#ifndef KSTEP_H
#define KSTEP_H

#include <linux/cpumask.h>
#include <linux/types.h>
#include <linux/version.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#include <kernel/sched/sched.h> // internal header
#pragma GCC diagnostic pop

#include "logging.h"

#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

// main.c
extern struct kstep_driver *kstep_driver;

// driver.c
struct kstep_driver {
  const char *name;         // Name of the driver
  void (*setup)(void);      // Setup the driver (e.g., create tasks)
  void (*run)(void);        // Run the driver
  u64 step_interval_us;     // Time between steps in us (default: 10000)
  bool print_rq;            // Print runqueue stats
  bool print_tasks;         // Print task stats
  bool print_nr_running;    // Print number of running tasks
  bool print_load_balance;  // Print load balancing
  bool print_sched_softirq; // Print sched softirq latency
};
struct kstep_driver *kstep_driver_get(const char *name);
void kstep_driver_print(struct kstep_driver *driver);

// tick.c
extern u64 kstep_tick_count;
void kstep_tick_init(void);
void kstep_tick_exit(void);
void kstep_sleep(void);
void kstep_tick(void);
void kstep_tick_repeat(int n);
void *kstep_tick_until(void *(*fn)(void));

// task.c
void kstep_tasks_init(void);
struct task_struct *kstep_task_create(void);
void kstep_task_pin(struct task_struct *p, int begin, int end);
void kstep_task_fork(struct task_struct *p, int n);
void kstep_task_fifo(struct task_struct *p);
void kstep_task_pause(struct task_struct *p);
void kstep_task_wakeup(struct task_struct *p);
void kstep_task_sleep(struct task_struct *p, int n);
void kstep_task_set_prio(struct task_struct *p, int prio);

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
void kstep_print_rq(void);
void kstep_print_tasks(void);
void kstep_print_nr_running(void);
void kstep_trace_sched_softirq(void);
void kstep_trace_load_balance(void);

// reset.c
void kstep_reset_sched(void);
void kstep_reset_task(struct task_struct *p);

// isolation.c
void kstep_disable_workqueue(void);
void kstep_move_kthreads(void);
void kstep_prealloc_kworkers(void);
bool kstep_is_sys_kthread(struct task_struct *p);

// topo.c
enum kstep_topo_type {
  KSTEP_TOPO_SMT,
  KSTEP_TOPO_CLS,
  KSTEP_TOPO_MC,
  KSTEP_TOPO_PKG,
  KSTEP_TOPO_NODE,
  KSTEP_TOPO_NR,
};
void kstep_topo_init(void);
void kstep_topo_set_level(enum kstep_topo_type type, const char *cpulists[],
                          int size);
void kstep_topo_apply(void);
void kstep_topo_print(void);
void kstep_cpu_set_freq(int cpu, int scale);
void kstep_cpu_set_capacity(int cpu, int scale);

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
