/*
 * Reproducer for: __setscheduler_uclamp uses rt_task(p) which includes
 * PI-boosted tasks, can set wrong uclamp.min for SCHED_NORMAL.
 *
 * In __setscheduler_uclamp(), rt_task(p) checks p->prio (which reflects
 * PI boosting) rather than p->policy. When sched_setattr() is called on
 * a SCHED_NORMAL task that is PI-boosted to RT priority, rt_task(p) returns
 * true and uclamp_req[UCLAMP_MIN] is incorrectly set to 1024 (RT default).
 * After the PI boost is removed, the SCHED_NORMAL task is left with a
 * permanently wrong max-boost uclamp.min.
 *
 * The fix: use task_has_rt_policy(p) instead of rt_task(p).
 *
 * PI-boost mechanism:
 * In the real-world scenario, a SCHED_NORMAL task holds an rt_mutex while a
 * SCHED_FIFO task contends for it, causing the kernel to boost the holder's
 * priority via rt_mutex_setprio(). kSTEP kthreads use a predefined action
 * model (spin/yield/block-on-waitqueue) and cannot execute arbitrary code such
 * as rt_mutex_lock(), so we call rt_mutex_setprio() directly to simulate the
 * PI boost. This is functionally equivalent because rt_mutex_setprio() is the
 * same internal function the kernel's PI mechanism invokes when resolving
 * priority inheritance during actual mutex contention.
 *
 * Requires CONFIG_RT_MUTEXES=y and CONFIG_UCLAMP_TASK=y.
 */

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

#include <linux/sched/rt.h>
#include <linux/rtmutex.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>

#include "driver.h"
#include "internal.h"

#ifdef CONFIG_RT_MUTEXES

KSYM_IMPORT(rt_mutex_setprio);

/*
 * Declared to document the intended contention path. In a real system, victim
 * would hold this mutex and donor would block on it, triggering PI boosting.
 * See file-level comment for why we call rt_mutex_setprio() directly instead.
 */
static DEFINE_RT_MUTEX(pi_mutex);

static struct task_struct *victim;
static struct task_struct *donor;

static void setup(void)
{
	/* victim: SCHED_NORMAL kthread on CPU 1 (receives PI boost) */
	victim = kstep_kthread_create("victim");
	kstep_kthread_bind(victim, cpumask_of(1));

	/* donor: SCHED_FIFO kthread on CPU 1 (PI boost source) */
	donor = kstep_kthread_create("donor");
	kstep_kthread_bind(donor, cpumask_of(1));

	struct sched_attr donor_attr = {
		.size = sizeof(struct sched_attr),
		.sched_policy = SCHED_FIFO,
		.sched_priority = 1,
	};
	sched_setattr_nocheck(donor, &donor_attr);
}

static void run(void)
{
	(void)pi_mutex;

	kstep_kthread_start(victim);
	kstep_kthread_start(donor);
	kstep_kthread_yield(victim);
	kstep_kthread_yield(donor);
	kstep_tick_repeat(3);

	unsigned int initial_min = victim->uclamp_req[UCLAMP_MIN].value;
	TRACE_INFO("Initial: pid=%d policy=%d prio=%d normal_prio=%d "
		   "uclamp_req[MIN]=%u rt_task=%d",
		   victim->pid, victim->policy, victim->prio,
		   victim->normal_prio, initial_min, rt_task(victim));

	/*
	 * Apply PI boost: rt_mutex_setprio() is called by the kernel's PI
	 * mechanism when an RT task (donor) blocks on a mutex held by a
	 * lower-priority task (victim). We invoke it directly here.
	 */
	raw_spin_lock_irq(&victim->pi_lock);
	KSYM_rt_mutex_setprio(victim, donor);
	raw_spin_unlock_irq(&victim->pi_lock);

	TRACE_INFO("After PI boost: policy=%d prio=%d normal_prio=%d "
		   "rt_task=%d task_has_rt_policy=%d",
		   victim->policy, victim->prio, victim->normal_prio,
		   rt_task(victim), task_has_rt_policy(victim));

	/*
	 * Change victim's nice value while PI-boosted. This triggers
	 * __setscheduler_uclamp() which incorrectly checks rt_task(p)
	 * instead of task_has_rt_policy(p).
	 */
	struct sched_attr attr = {
		.size = sizeof(struct sched_attr),
		.sched_policy = SCHED_NORMAL,
		.sched_nice = 5,
	};
	int ret = sched_setattr_nocheck(victim, &attr);
	TRACE_INFO("sched_setattr_nocheck returned %d", ret);

	unsigned int after_setattr_min = victim->uclamp_req[UCLAMP_MIN].value;
	TRACE_INFO("After setattr (still PI-boosted): policy=%d prio=%d "
		   "uclamp_req[MIN]=%u",
		   victim->policy, victim->prio, after_setattr_min);

	/* Remove PI boost */
	raw_spin_lock_irq(&victim->pi_lock);
	KSYM_rt_mutex_setprio(victim, NULL);
	raw_spin_unlock_irq(&victim->pi_lock);

	unsigned int final_min = victim->uclamp_req[UCLAMP_MIN].value;
	TRACE_INFO("After de-boost: policy=%d prio=%d normal_prio=%d "
		   "uclamp_req[MIN]=%u",
		   victim->policy, victim->prio, victim->normal_prio,
		   final_min);

	/*
	 * Bug check: A SCHED_NORMAL task should have uclamp_req[MIN] = 0.
	 * Due to the bug, it gets set to 1024 (SCHED_CAPACITY_SCALE) during
	 * the PI-boosted sched_setattr call and is never corrected.
	 */
	if (victim->policy == SCHED_NORMAL && final_min == SCHED_CAPACITY_SCALE) {
		kstep_fail("SCHED_NORMAL task pid=%d has uclamp_req[MIN]=%u "
			   "(expected 0) after PI-boost + sched_setattr",
			   victim->pid, final_min);
	} else if (victim->policy == SCHED_NORMAL && final_min == 0) {
		kstep_pass("uclamp_req[MIN] correctly 0 for SCHED_NORMAL task "
			   "after PI-boost + sched_setattr");
	} else {
		kstep_fail("unexpected state: policy=%d uclamp_req[MIN]=%u",
			   victim->policy, final_min);
	}
}

#else /* !CONFIG_RT_MUTEXES */

static void setup(void) {}

static void run(void)
{
	kstep_fail("CONFIG_RT_MUTEXES not enabled - cannot test PI boosting");
}

#endif /* CONFIG_RT_MUTEXES */

KSTEP_DRIVER_DEFINE {
	.name = "uclamp_fork_reset_rt_boost",
	.setup = IS_ENABLED(CONFIG_RT_MUTEXES) ? setup : NULL,
	.run = run,
	.step_interval_us = 1000,
};

#else /* LINUX_VERSION_CODE < 6.19.0 */

#include "driver.h"

static void run(void)
{
	kstep_pass("skipped: requires kernel >= 6.19");
}

KSTEP_DRIVER_DEFINE {
	.name = "uclamp_fork_reset_rt_boost",
	.run = run,
};

#endif
