#ifndef KSTEP_INTERNAL_H
#define KSTEP_INTERNAL_H

#include <linux/cpumask.h>
#include <linux/cpuset.h>
#include <linux/freezer.h>
#include <linux/sched/clock.h>
#include <linux/types.h>
#include <linux/version.h>

// kernel internal headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#include <kernel/sched/sched.h>
#include <kernel/time/tick-internal.h>
#include <kernel/time/tick-sched.h>
#pragma GCC diagnostic pop

#include "driver.h"

#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s

// main.c
extern struct kstep_driver *kstep_driver;

// driver.c
struct kstep_driver *kstep_driver_get(const char *name);

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
void kstep_ksym_init(void);

// Macros to import a kernel symbol `foo` as a POINTER `KSYM_foo`.
// - KSYM_IMPORT(name) can be used if the type of the symbol is already known
//   from some header file.
// - KSYM_IMPORT_TYPED(type, name) allows manual type specification.
#define KSYM_IMPORT(name) KSYM_IMPORT_TYPED(typeof(name), name)
#define KSYM_IMPORT_TYPED(type, name) static KSYM_IMPORT_RAW(type, name) __used
#define KSYM_IMPORT_RAW(type, name) type *KSYM_##name

extern KSYM_IMPORT_RAW(struct rq, runqueues);
#undef cpu_rq
#define cpu_rq(cpu) (per_cpu_ptr(KSYM_runqueues, (cpu)))
#undef this_rq
#define this_rq() this_cpu_ptr(KSYM_runqueues)
#undef raw_rq
#define raw_rq() raw_cpu_ptr(KSYM_runqueues)

#endif
