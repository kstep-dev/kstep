// Reproduce: wrong CPU's has_idle_cores cleared in select_idle_cpu()
// (commit 02dbb7246c5b)
//
// Bug: set_idle_cores(this, false) should be set_idle_cores(target, false).
// When this != target (cross-LLC wakeup), the wrong LLC's has_idle_cores
// flag is cleared.
//
// Topology (7 CPUs): SMT {1-2},{3-4},{5-6}; MC {1-4},{5-6}
// LLC A (sd_llc for CPU 1) = MC domain {1-4}
// LLC B (sd_llc for CPU 5) = SMT domain {5-6} (MC degenerated)
// Waker kthread on CPU 1 (LLC A) wakes blocked kthread (prev_cpu=5, LLC B)
// All CPUs in LLC B busy -> select_idle_cpu finds no idle core
// Bug: clears has_idle_cores for LLC A (this=1). Fix: clears LLC B (target=5)

#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/sched/smt.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 13, 0)

KSYM_IMPORT(sd_llc_shared);
KSYM_IMPORT(sched_smt_present);

static struct task_struct *busy5;
static struct task_struct *busy6;
static struct task_struct *wakee;
static struct task_struct *waker;

static void setup(void)
{
kstep_topo_init();

const char *smt[] = {"0", "1-2", "1-2", "3-4", "3-4", "5-6", "5-6"};
kstep_topo_set_smt(smt, ARRAY_SIZE(smt));

const char *mc[] = {"0", "1-4", "1-4", "1-4", "1-4", "5-6", "5-6"};
kstep_topo_set_mc(mc, ARRAY_SIZE(mc));

kstep_topo_apply();

// QEMU vCPUs have no real SMT; enable the static key manually
static_branch_enable(KSYM_sched_smt_present);

busy5 = kstep_kthread_create("busy5");
kstep_kthread_bind(busy5, cpumask_of(5));
kstep_kthread_start(busy5);

busy6 = kstep_kthread_create("busy6");
kstep_kthread_bind(busy6, cpumask_of(6));
kstep_kthread_start(busy6);

wakee = kstep_kthread_create("wakee");
kstep_kthread_bind(wakee, cpumask_of(5));
kstep_kthread_start(wakee);

waker = kstep_kthread_create("waker");
kstep_kthread_bind(waker, cpumask_of(1));
kstep_kthread_start(waker);
}

static void run(void)
{
kstep_tick_repeat(5);

kstep_kthread_block(wakee);
kstep_tick_repeat(5);

TRACE_INFO("sched_smt_active=%d", sched_smt_active());

struct sched_domain_shared *sds_b = rcu_dereference(
*per_cpu_ptr(KSYM_sd_llc_shared, 5));
struct sched_domain_shared *sds_a = rcu_dereference(
*per_cpu_ptr(KSYM_sd_llc_shared, 1));

if (!sds_b || !sds_a) {
kstep_fail("sd_llc_shared NULL: a=%px b=%px", sds_a, sds_b);
return;
}

if (sds_b == sds_a) {
kstep_fail("LLCs share same sds (topology wrong)");
return;
}

TRACE_INFO("wakee task_cpu=%d, waker task_cpu=%d",
   task_cpu(wakee), task_cpu(waker));
TRACE_INFO("rq[5].nr_running=%d rq[6].nr_running=%d",
   cpu_rq(5)->nr_running, cpu_rq(6)->nr_running);

WRITE_ONCE(sds_b->has_idle_cores, 1);
WRITE_ONCE(sds_a->has_idle_cores, 1);

TRACE_INFO("Before wakeup: LLC_B=%d LLC_A=%d",
   READ_ONCE(sds_b->has_idle_cores),
   READ_ONCE(sds_a->has_idle_cores));

kstep_kthread_syncwake(waker, wakee);
kstep_tick_repeat(3);

int has_idle_b = READ_ONCE(sds_b->has_idle_cores);
int has_idle_a = READ_ONCE(sds_a->has_idle_cores);

TRACE_INFO("After wakeup: LLC_B=%d LLC_A=%d", has_idle_b, has_idle_a);

if (has_idle_b == 1 && has_idle_a == 0) {
kstep_fail("Wrong LLC cleared: LLC_B=%d LLC_A=%d",
   has_idle_b, has_idle_a);
} else if (has_idle_b == 0) {
kstep_pass("LLC B correctly cleared: LLC_B=%d LLC_A=%d",
   has_idle_b, has_idle_a);
} else {
kstep_fail("Unexpected: LLC_B=%d LLC_A=%d", has_idle_b, has_idle_a);
}
}

KSTEP_DRIVER_DEFINE{
    .name = "has_idle_cores",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "has_idle_cores",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
