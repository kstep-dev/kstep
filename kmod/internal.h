#ifndef INTERNAL_H
#define INTERNAL_H

#include <linux/types.h>

#include "sigcode.h"

// Cannot be larger than DELAY_CONST_MAX
#define SIM_INTERVAL_US (19000ULL)
#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

// Forward declarations
struct task_struct;
struct cpumask;
struct rq;
struct sched_domain;

// sched_clock.c
void sched_clock_init(void);
void sched_clock_exit(void);
void sched_clock_set(u64 value);
void sched_clock_tick(void);

// utils.c
#define send_sigcode(p, code, val) send_sigcode3(p, code, val, 0, 0)
#define send_sigcode2(p, code, val1, val2) send_sigcode3(p, code, val1, val2, 0)
void send_sigcode3(struct task_struct *p, enum sigcode code, int val1, int val2,
                   int val3);
struct task_struct *poll_task(const char *comm);
void reset_task_stats(struct task_struct *p);
void print_tasks(void);

extern const struct cpumask *cpu_controlled_mask;
void cpu_controlled_mask_init(void);
#define for_each_controlled_cpu(cpu) for_each_cpu(cpu, cpu_controlled_mask)

int is_sys_kthread(struct task_struct *p);

// trace.c
int kstep_trace_init(void);
void kstep_trace_exit(void);
void kstep_make_function_noop(char *name);
void kstep_trace_rq_clock(void);
#endif
