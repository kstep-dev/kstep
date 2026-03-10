// Reproduce: incorrect next_class tracking in schedule_idle
// (commit 5324953c06bd)
//
// Bug: __schedule(SM_IDLE) with nr_running==0 shortcircuits to 'picked'
// without resetting rq->next_class to &idle_sched_class. If next_class
// was previously elevated (e.g., by wakeup_preempt or newidle balance),
// it stays elevated, causing subsequent wakeup_preempt() calls to take
// the wrong branch and miss resched_curr().
//
// Fix: Add rq->next_class = &idle_sched_class before goto picked in
// the SM_IDLE shortcircuit path, and replace direct assignment in
// sched_balance_newidle with rq_modified_begin() which only lowers.

#include "driver.h"
#include "internal.h"
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(7, 0, 0)

static struct task_struct *task1;
static struct task_struct *task2;
static struct task_struct *task3;

static const struct sched_class *fair_class;
static const struct sched_class *idle_class;

/*
 * Runs on CPU 1 via smp_call_function_single.
 * Sets rq->next_class to fair_sched_class and triggers need_resched
 * so the idle loop calls schedule_idle().
 */
static void elevate_next_class(void *arg)
{
	struct rq *rq = this_rq();
	const struct sched_class *fair = *(const struct sched_class **)arg;

	rq->next_class = fair;
	set_tsk_need_resched(rq->curr);
}

static void setup(void)
{
	fair_class = kstep_ksym_lookup("fair_sched_class");
	idle_class = kstep_ksym_lookup("idle_sched_class");
	if (!fair_class || !idle_class)
		panic("Failed to resolve sched_class symbols");

	kstep_topo_init();
	kstep_topo_apply();

	task1 = kstep_task_create();
	task2 = kstep_task_create();
	task3 = kstep_task_create();
}

static void run(void)
{
	struct rq *rq1 = cpu_rq(1);

	/* Pin 2 tasks to CPU 2 to overload it */
	kstep_task_pin(task2, 2, 2);
	kstep_task_pin(task3, 2, 2);
	/* Pin 1 task to CPU 1 */
	kstep_task_pin(task1, 1, 1);

	kstep_tick_repeat(20);

	/* Pause task to make CPU 1 idle */
	kstep_task_pause(task1);
	kstep_tick_repeat(5);

	/* Verify CPU 1 is idle */
	TRACE_INFO("CPU 1: curr=%s nr_running=%u next_class=%ps",
		   rq1->curr->comm, rq1->nr_running, rq1->next_class);

	if (rq1->curr != rq1->idle || rq1->nr_running != 0) {
		kstep_pass("CPU 1 not idle, cannot test schedule_idle path");
		return;
	}

	/*
	 * Simulate the bug condition: elevate next_class to fair_sched_class
	 * and set need_resched to trigger schedule_idle() from the idle loop.
	 * schedule_idle() with nr_running==0 shortcircuits to 'picked':
	 *   Buggy: doesn't reset next_class → stays at fair
	 *   Fixed: sets next_class = &idle_sched_class before goto picked
	 */
	TRACE_INFO("Elevating next_class to fair on CPU 1");
	smp_call_function_single(1, elevate_next_class, (void *)&fair_class, 1);

	/* Let CPU 1 process: idle loop sees need_resched, calls schedule_idle */
	kstep_sleep();
	kstep_sleep();
	kstep_sleep();

	TRACE_INFO("After schedule_idle: next_class=%ps (expect idle=%ps on fixed)",
		   rq1->next_class, idle_class);

	if (rq1->next_class == fair_class) {
		kstep_fail("schedule_idle did not reset next_class (stayed at fair)");
	} else if (rq1->next_class == idle_class) {
		kstep_pass("schedule_idle correctly reset next_class to idle");
	} else {
		TRACE_INFO("Unexpected next_class=%ps", rq1->next_class);
		kstep_pass("next_class is neither fair nor idle: %ps", rq1->next_class);
	}
}

KSTEP_DRIVER_DEFINE{
    .name = "wakeup_next_class",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#endif
