#ifndef KSTEP_INTERNAL_H
#define KSTEP_INTERNAL_H

#include <linux/cpumask.h>
#include <linux/types.h>
#include <linux/version.h>

// kernel internal headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#include <kernel/sched/sched.h>
#include <kernel/time/tick-sched.h>
#pragma GCC diagnostic pop

#include "driver.h"

#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

// main.c
extern struct kstep_driver *kstep_driver;

// driver.c
struct kstep_driver *kstep_driver_get(const char *name);
void kstep_driver_print(struct kstep_driver *driver);

// tick.c
void kstep_sched_timer_init(void);
void kstep_jiffies_init(void);
void kstep_sched_clock_init(void);

// output.c
void kstep_print_rq(void);
void kstep_print_tasks(void);
void kstep_print_nr_running(void);
void kstep_trace_sched_softirq(void);
void kstep_trace_load_balance(void);

// reset.c
void kstep_reset_runqueues(void);
void kstep_reset_cpumask(void);
void kstep_reset_tasks(void);
void kstep_patch_min_vruntime(void);

// isolation.c
void kstep_disable_workqueue(void);
void kstep_move_kthreads(void);
void kstep_prealloc_kworkers(void);
bool kstep_is_sys_kthread(struct task_struct *p);

// ksym.c
struct ksym {
#define KSYM_FUNC(ret_type, name, ...) ret_type (*name)(__VA_ARGS__);
#define KSYM_VAR(type, name) type *name;
#include "ksym.h"
#undef KSYM_FUNC
#undef KSYM_VAR
};
extern struct ksym ksym;
void ksym_init(void);

#undef cpu_rq
#define cpu_rq(cpu) (per_cpu_ptr(ksym.runqueues, (cpu)))
#undef this_rq
#define this_rq() this_cpu_ptr(ksym.runqueues)
#undef raw_rq
#define raw_rq() raw_cpu_ptr(ksym.runqueues)

#endif
