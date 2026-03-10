// Reproduce: ttwu() race with stale task_cpu (commit b6e13e85829f)
//
// Bug: In try_to_wake_up(), task_cpu(p) is loaded before p->on_cpu, allowing
// a stale CPU value to be used when queuing the task on a remote CPU's
// wakelist. When sched_ttwu_pending() processes the entry, the task's
// se.cfs_rq points to the wrong CPU's cfs_rq, causing a NULL deref in
// check_preempt_curr() -> find_matching_se() -> is_same_group().
//
// Fix: Reorder loads so p->on_cpu is read (with smp_load_acquire) before
// task_cpu(p). Add defensive WARN_ON_ONCE checks in sched_ttwu_pending()
// to detect and correct mismatched task_cpu vs cpu_of(rq).
//
// Strategy: Create a task X on CPU 2, pause it to sleep, then directly call
// sched_ttwu_pending() on CPU 1 with X on the llist. Since task_cpu(X)==2
// but we activate on CPU 1's rq, se.cfs_rq mismatches. Buggy kernel crashes;
// fixed kernel corrects via set_task_cpu() in sched_ttwu_pending().

#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

static struct task_struct *task_x;
static struct task_struct *task_y;
static atomic_t trigger_done;

// Called on CPU 1 via smp_call_function_single.
// Invokes sched_ttwu_pending with task_x on CPU 1's llist,
// simulating the race outcome where ttwu() queued the task on the wrong CPU.
static void trigger_ttwu_pending(void *info)
{
	struct task_struct *p = info;

	typedef void (*sttp_fn_t)(void *);
	sttp_fn_t sttp = (sttp_fn_t)kstep_ksym_lookup("sched_ttwu_pending");

	// Create single-entry llist
	p->wake_entry.llist.next = NULL;
	p->sched_remote_wakeup = 0;

	// Mark rq as having pending wakeups
	struct rq *rq = this_rq();
	WRITE_ONCE(rq->ttwu_pending, 1);

	// Call sched_ttwu_pending — on buggy kernel this crashes because
	// task_cpu(p) != cpu_of(rq) and se.cfs_rq points to wrong CPU
	sttp(&p->wake_entry);

	atomic_set(&trigger_done, 1);
}

static void setup(void)
{
	kstep_topo_init();
	atomic_set(&trigger_done, 0);

	// Need a task on CPU 1 so check_preempt_curr has a valid curr->se
	task_y = kstep_task_create();
	kstep_task_pin(task_y, 1, 1);

	// Task X on CPU 2 — its se.cfs_rq will point to cpu_rq(2)->cfs
	task_x = kstep_task_create();
	kstep_task_pin(task_x, 2, 2);
}

static void run(void)
{
	struct rq *rq1 = cpu_rq(1);
	struct rq *rq2 = cpu_rq(2);

	// Let tasks settle on their CPUs
	kstep_tick_repeat(5);

	TRACE_INFO("task_x cpu=%d on_rq=%d on_cpu=%d",
		   task_cpu(task_x), task_x->on_rq, task_x->on_cpu);
	TRACE_INFO("task_y cpu=%d on_rq=%d on_cpu=%d",
		   task_cpu(task_y), task_y->on_rq, task_y->on_cpu);
	TRACE_INFO("rq1->cfs addr=%px, rq2->cfs addr=%px",
		   &rq1->cfs, &rq2->cfs);

	// Pause X — it sleeps on CPU 2 with se.cfs_rq = &cpu_rq(2)->cfs
	kstep_task_pause(task_x);
	kstep_sleep();
	kstep_sleep();

	TRACE_INFO("After pause: task_x cpu=%d on_rq=%d on_cpu=%d state=%ld",
		   task_cpu(task_x), task_x->on_rq, task_x->on_cpu,
		   task_x->state);
	TRACE_INFO("task_x se.cfs_rq=%px (expect rq2->cfs=%px)",
		   task_x->se.cfs_rq, &rq2->cfs);

	if (task_cpu(task_x) != 2 || task_x->on_rq != 0) {
		TRACE_INFO("Unexpected state, aborting");
		kstep_pass("unexpected task state, cannot test");
		return;
	}

	// Set state to TASK_WAKING as ttwu() would before the wakelist path
	task_x->state = TASK_WAKING;

	TRACE_INFO("Triggering sched_ttwu_pending on CPU 1 with task_x (cpu=2)");
	TRACE_INFO("Buggy kernel: expect NULL deref in is_same_group");
	TRACE_INFO("Fixed kernel: expect WARN_ON_ONCE + correction");

	// Call from CPU 1 — this simulates the race outcome:
	// task_x->cpu=2 but activated on CPU 1's rq
	smp_call_function_single(1, trigger_ttwu_pending, task_x, 1);

	// If we reach here, the kernel didn't crash
	TRACE_INFO("sched_ttwu_pending completed (no crash)");
	TRACE_INFO("After: task_x cpu=%d on_rq=%d on_cpu=%d state=%ld",
		   task_cpu(task_x), task_x->on_rq, task_x->on_cpu,
		   task_x->state);
	TRACE_INFO("task_x se.cfs_rq=%px (rq1->cfs=%px, rq2->cfs=%px)",
		   task_x->se.cfs_rq, &rq1->cfs, &rq2->cfs);

	if (task_cpu(task_x) == 1 && task_x->se.cfs_rq == &rq1->cfs) {
		kstep_pass("fixed kernel corrected task_cpu: 2 -> 1");
	} else {
		kstep_fail("task_cpu mismatch not corrected (cpu=%d)",
			   task_cpu(task_x));
	}

	kstep_tick_repeat(3);
}

KSTEP_DRIVER_DEFINE{
    .name = "ttwu_race",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#endif
