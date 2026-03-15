/*
 * Reproduce: set_delayed() modifies h_nr_runnable without updating
 * ancestor group entities' runnable_weight via se_update_runnable().
 *
 * When a task is delay-dequeued, set_delayed() walks every ancestor
 * cfs_rq and decrements h_nr_runnable, but never calls
 * se_update_runnable() for the group SEs. dequeue_entities() returns
 * -1 for delayed tasks, skipping the second loop that would fix it.
 * entity_tick() also omits se_update_runnable(). Result: stale
 * runnable_weight feeds into PELT via se_runnable() across ticks.
 *
 * Reference: 6212437f0f60 (original throttle fix), 3429dd57f0de
 */

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

static struct task_struct *target;
static struct task_struct *filler;

static void setup(void)
{
	kstep_cgroup_create("grp");

	target = kstep_task_create();
	filler = kstep_task_create();

	kstep_cgroup_add_task("grp", target->pid);
	kstep_cgroup_add_task("grp", filler->pid);
}

/*
 * Wait for target to be the current task on its CPU and ineligible
 * on its cfs_rq (the group cfs_rq). This is the condition that
 * triggers DELAY_DEQUEUE when the task goes to sleep.
 */
static void *target_ineligible(void)
{
	if (target->on_cpu &&
	    !target->se.sched_delayed &&
	    !kstep_eligible(&target->se))
		return target;
	return NULL;
}

static int stale_ticks;

static void on_tick_begin(void)
{
	struct sched_entity *gse = target->se.parent;

	if (!gse || !gse->my_q || !gse->on_rq)
		return;

	long rw = gse->runnable_weight;
	long h_nr = gse->my_q->h_nr_runnable;

	TRACE_INFO("tick: rw=%ld h_nr=%ld delayed=%d",
		   rw, h_nr, target->se.sched_delayed);

	if (target->se.sched_delayed && rw != h_nr)
		stale_ticks++;
}

static void run(void)
{
	kstep_task_wakeup(target);
	kstep_task_wakeup(filler);

	/* Let tasks accumulate runtime so one becomes ineligible */
	kstep_tick_repeat(5);

	/* Wait for target to be running and ineligible */
	struct task_struct *p = kstep_tick_until(target_ineligible);
	if (!p) {
		kstep_fail("target never became ineligible");
		return;
	}

	struct sched_entity *gse = target->se.parent;
	if (!gse || !gse->my_q) {
		kstep_fail("no group sched_entity");
		return;
	}

	long rw_before = gse->runnable_weight;
	long h_nr_before = gse->my_q->h_nr_runnable;

	TRACE_INFO("pre-pause: rw=%ld h_nr=%ld on_rq=%d",
		   rw_before, h_nr_before, gse->on_rq);

	/* Pause target: triggers dequeue_task_fair -> delay dequeue */
	kstep_task_pause(target);

	long rw_after = gse->runnable_weight;
	long h_nr_after = gse->my_q->h_nr_runnable;
	int delayed = target->se.sched_delayed;

	TRACE_INFO("post-pause: rw=%ld h_nr=%ld delayed=%d on_rq=%d",
		   rw_after, h_nr_after, delayed, gse->on_rq);

	if (!delayed) {
		TRACE_INFO("task not delay-dequeued; invariant not testable");
		kstep_pass("task not delayed; no invariant violation possible");
		kstep_tick_repeat(10);
		return;
	}

	/*
	 * The invariant: for an on_rq group entity, runnable_weight must
	 * equal its my_q->h_nr_runnable. set_delayed() decremented
	 * h_nr_runnable but did not call se_update_runnable().
	 */
	if (rw_after != h_nr_after) {
		TRACE_INFO("BUG: stale runnable_weight %ld != h_nr_runnable %ld",
			   rw_after, h_nr_after);
	}

	/* Tick a few more times; stale value persists on buggy kernels */
	stale_ticks = 0;
	kstep_tick_repeat(10);

	TRACE_INFO("stale_ticks=%d (ticks with rw != h_nr while delayed)",
		   stale_ticks);

	if (rw_after != h_nr_after) {
		kstep_fail("runnable_weight %ld != h_nr_runnable %ld "
			   "(stale for %d ticks)",
			   rw_after, h_nr_after, stale_ticks);
	} else {
		kstep_pass("runnable_weight %ld == h_nr_runnable %ld",
			   rw_after, h_nr_after);
	}

	kstep_tick_repeat(5);
}

KSTEP_DRIVER_DEFINE{
	.name = "bandwidth_throttle_runnable_avg",
	.setup = setup,
	.run = run,
	.on_tick_begin = on_tick_begin,
	.step_interval_us = 1000,
};

#endif /* KERNEL_VERSION >= 6.19 */
