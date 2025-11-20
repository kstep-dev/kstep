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
#include "sigcode.h"

#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

// main.c
struct kstep_params_t {
  char controller[32];                 // Name of the controller to run
  unsigned long long step_interval_us; // Interval between steps in us
  bool special_topo;                   // Whether to use the special topology
  bool print_tasks;                    // Whether to print tasks
  bool print_nr_running;               // Whether to print nr_running
};
extern struct kstep_params_t kstep_params;
void kstep_params_print(void);

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
void call_tick_once(void);

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
void print_nr_running(void);
int is_sys_kthread(struct task_struct *p);

// Call tick until the function returns true
void kstep_tick_until(bool (*fn)(void));
// Call tick until the function returns true for ANY task in some runqueue
struct task_struct *kstep_tick_until_task(bool (*fn)(struct task_struct *));

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
