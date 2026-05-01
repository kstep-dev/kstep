#ifndef KSTEP_INTERNAL_H
#define KSTEP_INTERNAL_H

#include <linux/types.h>
#include <linux/version.h>
#include <linux/compiler_types.h>

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

// tick.c
void kstep_tick_init(void);

// tick_clock.c
void kstep_sched_clock_init(void);
void kstep_sched_clock_tick(void);
u64 kstep_sched_clock_get(void);

// tick_jiffies.c
void kstep_jiffies_init(void);
void kstep_jiffies_tick(void);
u64 kstep_jiffies_get(void);

// output.c
void kstep_output_init(void);

// trace.c
void kstep_trace_sched_balance_begin(void);
void kstep_trace_sched_balance_selected(void);
void kstep_trace_sched_group_alloc(void);

// reset.c
void kstep_reset_runqueues(void);
void kstep_reset_cpumask(void);
void kstep_reset_tasks(void);
void kstep_reset_task(struct task_struct *p);
void kstep_reset_dl_server(void);

// isolation.c
void kstep_disable_workqueue(void);
void kstep_move_kthreads(void);
void kstep_prealloc_kworkers(void);

// task.c
void kstep_task_init(void);

// kernel.c
void kstep_cgroup_init(void);

// cov.c
void kstep_cov_init(void);
void kstep_cov_enable(void);
void kstep_cov_enable_controller(void);
void kstep_cov_disable_controller(void);
void kstep_cov_disable(void);
// void kstep_cov_dump_pcs(void);
// void kstep_cov_dump_signal(void);
void kstep_cov_dump(void);
void kstep_cov_cmd_id_inc(void);

// sym.c
struct kstep_driver *kstep_sym_init(const char *driver_name);
// Macros to import a kernel symbol `type foo` as a POINTER `type *KSYM_foo`.
// - KSYM_IMPORT(name) can be used if the type of the symbol is already known
//   from some header file.
// - KSYM_IMPORT_TYPED(type, name) allows manual type specification.
#define KSYM_IMPORT(name) KSYM_IMPORT_TYPED(typeof(name), name)
#define KSYM_IMPORT_TYPED(type, name) static KSYM_IMPORT_RAW(type, name) __used
#define KSYM_IMPORT_RAW(type, name) type *KSYM_##name
void *kstep_ksym_lookup(const char *name);

#ifndef __percpu
#define __percpu
#endif

extern KSYM_IMPORT_RAW(struct rq __percpu, runqueues);
#undef cpu_rq
#define cpu_rq(cpu) (per_cpu_ptr(KSYM_runqueues, (cpu)))
#undef this_rq
#define this_rq() this_cpu_ptr(KSYM_runqueues)
#undef raw_rq
#define raw_rq() raw_cpu_ptr(KSYM_runqueues)

#endif
