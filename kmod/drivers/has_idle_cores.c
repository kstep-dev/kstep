// Reproduce: wrong CPU's has_idle_cores cleared in select_idle_cpu()
// (commit 02dbb7246c5b)
//
// Bug: set_idle_cores(this, false) should be set_idle_cores(target, false).
// Topology (7 CPUs): SMT {1-2},{3-4},{5-6}; MC {1-4},{5-6}
// LLC A = MC {1-4}, LLC B = SMT {5-6} (MC degenerated)
// Waker on CPU 1 (LLC A) wakes blocked kthread (prev_cpu=5, LLC B)
// Bug: clears LLC A's has_idle_cores. Fix: clears LLC B's.

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
static_branch_enable(KSYM_sched_smt_present);

busy6 = kstep_kthread_create("busy6");
kstep_kthread_bind(busy6, cpumask_of(6));
kstep_kthread_start(busy6);

// Wakee starts on CPU 5 alone (will expand to {5,6} later)
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

// Block wakee -> wait_event -> sleep (prev_cpu = 5)
kstep_kthread_block(wakee);
kstep_tick_repeat(5);

// Expand wakee's affinity to {5,6} (LLC B) so nr_cpus_allowed > 1
// This ensures select_task_rq_fair is actually called during wakeup
struct cpumask llcb_mask;
cpumask_clear(&llcb_mask);
cpumask_set_cpu(5, &llcb_mask);
cpumask_set_cpu(6, &llcb_mask);
kstep_kthread_bind(wakee, &llcb_mask);

// Start busy5 on CPU 5 (idle loop picks it up)
busy5 = kstep_kthread_create("busy5");
kstep_kthread_bind(busy5, cpumask_of(5));
kstep_kthread_start(busy5);
kstep_tick_repeat(2);

TRACE_INFO("rq5_nr=%d rq6_nr=%d wakee_cpus=%d",
   cpu_rq(5)->nr_running, cpu_rq(6)->nr_running,
   wakee->nr_cpus_allowed);

struct sched_domain_shared *sds_b = rcu_dereference(
*per_cpu_ptr(KSYM_sd_llc_shared, 5));
struct sched_domain_shared *sds_a = rcu_dereference(
*per_cpu_ptr(KSYM_sd_llc_shared, 1));

if (!sds_b || !sds_a || sds_b == sds_a) {
kstep_fail("Bad topology: a=%px b=%px", sds_a, sds_b);
return;
}

WRITE_ONCE(sds_b->has_idle_cores, 1);
WRITE_ONCE(sds_a->has_idle_cores, 1);

TRACE_INFO("Before: LLC_B=%d LLC_A=%d smt=%d",
   READ_ONCE(sds_b->has_idle_cores),
   READ_ONCE(sds_a->has_idle_cores),
   sched_smt_active());

kstep_kthread_syncwake(waker, wakee);
mdelay(500);

int has_idle_b = READ_ONCE(sds_b->has_idle_cores);
int has_idle_a = READ_ONCE(sds_a->has_idle_cores);

TRACE_INFO("After: LLC_B=%d LLC_A=%d", has_idle_b, has_idle_a);

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
