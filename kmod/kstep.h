#ifndef KSTEP_H
#define KSTEP_H

#include <linux/types.h>

#include "ksym.h"
#include "logging.h"
#include "sigcode.h"

#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

// Forward declarations
struct task_struct;
struct cpumask;
struct rq;
struct sched_domain;

// main.c
struct kstep_params_t {
  char controller[32];                 // Name of the controller to run
  unsigned long long step_interval_us; // Interval between steps in us
  bool special_topo;                   // Whether to use the special topology
};
extern struct kstep_params_t kstep_params;

// controller.c
struct controller_ops {
  const char *name;
  void (*pre_init)(void);
  void (*init)(void);
  void (*body)(void);
};
void kstep_sleep(void);
struct controller_ops *kstep_controller_get(const char *name);
void kstep_controller_run(struct controller_ops *ops);
void call_tick_once(bool print_tasks_flag);

// clock.c
void kstep_clock_init(u64 init_time_ns);
void kstep_clock_tick(void);
void kstep_clock_exit(void);

// utils.c
#define send_sigcode(p, code, val) send_sigcode3(p, code, val, 0, 0)
#define send_sigcode2(p, code, val1, val2) send_sigcode3(p, code, val1, val2, 0)
void send_sigcode3(struct task_struct *p, enum sigcode code, int val1, int val2,
                   int val3);
struct task_struct *poll_task(const char *comm);
void reset_task_stats(struct task_struct *p);
void print_tasks(void);
int is_sys_kthread(struct task_struct *p);

// trace.c
void kstep_trace_exit(void);
void kstep_patch_func_noop(char *name);
void kstep_trace_rq_clock(void);
void kstep_trace_lb(void);
void kstep_trace_rebalance(void);
void kstep_patch_min_vruntime(void);

// topo.c
void kstep_topo_print(void);
void kstep_use_special_topo(void);
#endif
