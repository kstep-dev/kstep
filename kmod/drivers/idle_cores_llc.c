// Reproduce: wrong CPU's has_idle_cores cleared in select_idle_cpu()
// (commit 02dbb7246c5b)
//
// Bug: set_idle_cores(this, false) should be set_idle_cores(target, false).
// Topology (7 CPUs): SMT {1-2},{3-4},{5-6}; MC {1-4},{5-6}
// LLC A = MC {1-4}, LLC B = MC {5-6}
// Waker on CPU 1 (LLC A) wakes blocked kthread (prev_cpu=5, LLC B).
// All CPUs kept busy to prevent update_idle_core from re-setting flags.
// Bug: clears LLC A has_idle_cores. Fix: clears LLC B.

#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/sched/smt.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 13, 0)

KSYM_IMPORT(sd_llc_shared);
KSYM_IMPORT(sched_smt_present);

static struct task_struct *busy2, *busy3, *busy4;
static struct task_struct *busy5, *busy6;
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

static_branch_enable(KSYM_sched_smt_present);

// Keep all LLC A CPUs busy to prevent update_idle_core re-setting flag
busy2 = kstep_kthread_create("busy2");
kstep_kthread_bind(busy2, cpumask_of(2));
kstep_kthread_start(busy2);

busy3 = kstep_kthread_create("busy3");
kstep_kthread_bind(busy3, cpumask_of(3));
kstep_kthread_start(busy3);

busy4 = kstep_kthread_create("busy4");
kstep_kthread_bind(busy4, cpumask_of(4));
kstep_kthread_start(busy4);

busy6 = kstep_kthread_create("busy6");
kstep_kthread_bind(busy6, cpumask_of(6));
kstep_kthread_start(busy6);

// Wakee starts alone on CPU 5 so it can execute and block
wakee = kstep_kthread_create("wakee");
kstep_kthread_bind(wakee, cpumask_of(5));
kstep_kthread_start(wakee);

// Waker on CPU 1 (LLC A); stays spinning after wakeup
waker = kstep_kthread_create("waker");
kstep_kthread_bind(waker, cpumask_of(1));
kstep_kthread_start(waker);
}

static void run(void)
{
kstep_tick_repeat(5);

// Block wakee first (it's alone on CPU 5 so it gets to run)
kstep_kthread_block(wakee);
kstep_tick_repeat(5);
mdelay(50);

TRACE_INFO("wakee_state=%ld on_rq=%d rq5_nr=%d",
   wakee->state, wakee->on_rq, cpu_rq(5)->nr_running);

// Now start busy5 on CPU 5 (wakee is blocked, so busy5 gets the CPU)
busy5 = kstep_kthread_create("busy5");
kstep_kthread_bind(busy5, cpumask_of(5));
kstep_kthread_start(busy5);
kstep_tick_repeat(2);

// Expand wakee affinity to {5,6} so nr_cpus_allowed > 1
struct cpumask mask;
cpumask_clear(&mask);
cpumask_set_cpu(5, &mask);
cpumask_set_cpu(6, &mask);
set_cpus_allowed_ptr(wakee, &mask);

TRACE_INFO("sched_smt_active=%d nr_allowed=%d rq5=%d rq6=%d",
   sched_smt_active(), wakee->nr_cpus_allowed,
   cpu_rq(5)->nr_running, cpu_rq(6)->nr_running);

struct sched_domain_shared *sds_a = rcu_dereference(
*per_cpu_ptr(KSYM_sd_llc_shared, 1));
struct sched_domain_shared *sds_b = rcu_dereference(
*per_cpu_ptr(KSYM_sd_llc_shared, 5));

if (!sds_a || !sds_b || sds_a == sds_b) {
kstep_fail("Bad topology: a=%px b=%px", sds_a, sds_b);
return;
}

WRITE_ONCE(sds_a->has_idle_cores, 1);
WRITE_ONCE(sds_b->has_idle_cores, 1);

TRACE_INFO("Before: LLC_A=%d LLC_B=%d wakee_cpu=%d waker_cpu=%d",
   READ_ONCE(sds_a->has_idle_cores),
   READ_ONCE(sds_b->has_idle_cores),
   task_cpu(wakee), task_cpu(waker));

// Waker (CPU 1, LLC A) wakes wakee (prev_cpu=5, LLC B).
// wake_continue keeps waker spinning so CPU 1 stays busy.
kstep_kthread_wake_continue(waker, wakee);

// Wait for wakeup to complete (all CPUs busy, no idle core updates)
mdelay(200);

int has_idle_a = READ_ONCE(sds_a->has_idle_cores);
int has_idle_b = READ_ONCE(sds_b->has_idle_cores);

TRACE_INFO("After: LLC_A=%d LLC_B=%d wakee_state=%ld rq5=%d rq6=%d",
   has_idle_a, has_idle_b, READ_ONCE(wakee->state),
   cpu_rq(5)->nr_running, cpu_rq(6)->nr_running);

// Buggy: set_idle_cores(this=1, false) clears LLC_A, LLC_B stays 1
// Fixed: set_idle_cores(target=5, false) clears LLC_B, LLC_A stays 1
if (has_idle_b == 1 && has_idle_a == 0) {
kstep_fail("Wrong LLC cleared: LLC_A=%d LLC_B=%d",
   has_idle_a, has_idle_b);
} else if (has_idle_b == 0) {
kstep_pass("Target LLC correctly cleared: LLC_A=%d LLC_B=%d",
   has_idle_a, has_idle_b);
} else {
kstep_fail("Unexpected: LLC_A=%d LLC_B=%d", has_idle_a, has_idle_b);
}
}

KSTEP_DRIVER_DEFINE{
    .name = "idle_cores_llc",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "idle_cores_llc",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
