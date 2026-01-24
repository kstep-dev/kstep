#ifndef KSTEP_DRIVER_H
#define KSTEP_DRIVER_H

#include <generated/utsrelease.h>
#include <linux/sched.h>

#define TERM_GREEN "\033[92m"
#define TERM_RESET "\033[0m"
#define TRACE_INFO(fmt, ...)                                                   \
  pr_info(TERM_GREEN "%24s: " fmt TERM_RESET "\n", __func__, ##__VA_ARGS__)

// driver.c
struct kstep_driver {
  const char *name;         // Name of the driver
  void (*setup)(void);      // Setup the driver (e.g., create tasks)
  void (*run)(void);        // Run the driver
  u64 step_interval_us;     // Time between steps in us
  bool print_rq;            // Print runqueue stats
  bool print_tasks;         // Print task stats
  bool print_nr_running;    // Print number of running tasks
  bool print_load_balance;  // Print load balancing
  bool print_sched_softirq; // Print sched softirq latency
};

static inline void kstep_driver_print(struct kstep_driver *driver) {
  TRACE_INFO("- %-20s: %s", "Linux version", UTS_RELEASE);
  TRACE_INFO("- %-20s: %s", "Driver name", driver->name);
  TRACE_INFO("- %-20s: %llu", "step_interval_us", driver->step_interval_us);
  TRACE_INFO("- %-20s: %d", "print_rq", driver->print_rq);
  TRACE_INFO("- %-20s: %d", "print_tasks", driver->print_tasks);
  TRACE_INFO("- %-20s: %d", "print_nr_running", driver->print_nr_running);
  TRACE_INFO("- %-20s: %d", "print_load_balance", driver->print_load_balance);
  TRACE_INFO("- %-20s: %d", "print_sched_softirq", driver->print_sched_softirq);
}

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
void kstep_task_pause(struct task_struct *p);
void kstep_task_wakeup(struct task_struct *p);
void kstep_task_usleep(struct task_struct *p, int us);
void kstep_task_set_prio(struct task_struct *p, int prio);

// kernel.c
void kstep_write(const char *path, const char *buf, size_t size);
void kstep_mkdir(const char *dir);
void kstep_sysctl_write(const char *name, const char *fmt, ...);
void kstep_cgroup_write(const char *name, const char *filename, const char *fmt,
                        ...);
void kstep_cgroup_create_pinned(const char *name, const char *cpuset);
void kstep_cgroup_set_weight(const char *name, int weight);
void kstep_cgroup_add_task(const char *name, int pid);
void kstep_freeze_task(struct task_struct *p);
int kstep_eligible(struct sched_entity *se);

// topo.c
void kstep_topo_init(void);
enum kstep_topo_type {
  KSTEP_TOPO_SMT,
  KSTEP_TOPO_CLS,
  KSTEP_TOPO_MC,
  KSTEP_TOPO_PKG,
  KSTEP_TOPO_NODE,
  KSTEP_TOPO_NR,
};
void kstep_topo_set_level(enum kstep_topo_type type, const char *cpulists[],
                          int size);
void kstep_topo_apply(void);
void kstep_topo_print(void);
void kstep_cpu_set_freq(int cpu, int scale);
void kstep_cpu_set_capacity(int cpu, int scale);

#endif
