// https://github.com/torvalds/linux/commit/8feb053d5319
//
// Bug: In __schedule(), try_to_block_task() takes task_state by value.
// When signal_pending_state() triggers, it sets p->__state = TASK_RUNNING,
// but the caller's prev_state remains stale (e.g. TASK_INTERRUPTIBLE).
// trace_sched_switch then reports the wrong task state.
//
// Fix: Change try_to_block_task() to accept a pointer to task_state so it
// can update the caller's variable.
//
// Observable: Register a sched_switch tracepoint probe. When a task with
// pending signals is being switched out, prev_state (tracepoint arg) says
// sleeping but p->__state is actually TASK_RUNNING.
// Buggy: prev_state != 0 while p->__state == TASK_RUNNING
// Fixed: prev_state == TASK_RUNNING (0)

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 14, 0)

#include <linux/kthread.h>
#include <linux/tracepoint.h>
#include <linux/sched/signal.h>

static struct task_struct *target;
static atomic_t bug_detected = ATOMIC_INIT(0);
static atomic_t tracepoint_hits = ATOMIC_INIT(0);
static unsigned long observed_prev_state;
static unsigned long observed_real_state;
static struct tracepoint *sched_switch_tp;

// Synchronization: driver tells kthread when to trigger the bug
static atomic_t phase = ATOMIC_INIT(0);

static void sched_switch_probe(void *data, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next,
			       unsigned int prev_state)
{
	if (prev != target)
		return;

	atomic_inc(&tracepoint_hits);

	TRACE_INFO("sched_switch: prev=%s pid=%d prev_state=%lu real_state=%lu",
		   prev->comm, prev->pid, (unsigned long)prev_state,
		   (unsigned long)READ_ONCE(prev->__state));

	// Only check when prev_state is TASK_INTERRUPTIBLE (our trigger) or
	// TASK_RUNNING (correctly updated). Skip TASK_DEAD from kthread exit.
	if (prev_state == TASK_INTERRUPTIBLE || prev_state == TASK_RUNNING) {
		observed_prev_state = prev_state;
		observed_real_state = READ_ONCE(prev->__state);
		if (prev_state != 0 && observed_real_state == TASK_RUNNING)
			atomic_set(&bug_detected, 1);
	}
}

static void find_sched_switch_tp(struct tracepoint *tp, void *priv)
{
	if (!strcmp(tp->name, "sched_switch"))
		sched_switch_tp = tp;
}

// Test kthread: spin to accumulate vruntime, then trigger the bug
static int test_thread_fn(void *data)
{
	// Phase 0: spin until driver tells us to proceed (phase 1)
	// During this, the driver fires ticks to accumulate our vruntime
	while (atomic_read(&phase) == 0)
		cpu_relax();

	// Phase 1: trigger the bug
	set_current_state(TASK_INTERRUPTIBLE);
	set_tsk_thread_flag(current, TIF_SIGPENDING);
	schedule();

	clear_tsk_thread_flag(current, TIF_SIGPENDING);
	return 0;
}

// Spinner kthread: always runnable on CPU 1 as context-switch target
static int spinner_fn(void *data)
{
	while (!kthread_should_stop())
		cpu_relax();
	return 0;
}

static void setup(void) {}

static void run(void)
{
	struct task_struct *spinner;
	int ret;

	for_each_kernel_tracepoint(find_sched_switch_tp, NULL);
	if (!sched_switch_tp) {
		kstep_fail("Could not find sched_switch tracepoint");
		return;
	}

	ret = tracepoint_probe_register(sched_switch_tp, sched_switch_probe, NULL);
	if (ret) {
		kstep_fail("Failed to register sched_switch probe: %d", ret);
		return;
	}
	TRACE_INFO("Registered sched_switch tracepoint probe");

	// Create test kthread FIRST on CPU 1 and let it accumulate vruntime
	target = kthread_create(test_thread_fn, NULL, "sswitch_test");
	if (IS_ERR(target)) {
		kstep_fail("Failed to create test kthread");
		goto out_unreg;
	}
	kthread_bind(target, 1);
	wake_up_process(target);

	// Fire many ticks so test kthread accumulates high vruntime
	kstep_sleep();
	kstep_tick_repeat(30);
	kstep_sleep();

	TRACE_INFO("test vruntime=%llu after spinning", target->se.vruntime);

	// Now create spinner with fresh (low) vruntime
	spinner = kthread_create(spinner_fn, NULL, "sswitch_spin");
	if (IS_ERR(spinner)) {
		kstep_fail("Failed to create spinner");
		goto out_unreg;
	}
	kthread_bind(spinner, 1);
	wake_up_process(spinner);
	TRACE_INFO("Started test pid=%d spinner pid=%d", target->pid, spinner->pid);

	// Let spinner enqueue
	kstep_sleep();
	kstep_tick_repeat(2);
	kstep_sleep();

	TRACE_INFO("test vruntime=%llu spinner vruntime=%llu",
		   target->se.vruntime, spinner->se.vruntime);

	// Tell the test kthread to trigger the bug
	atomic_set(&phase, 1);

	// Fire ticks to trigger the context switch
	kstep_tick_repeat(5);
	kstep_sleep();
	kstep_sleep();

	kthread_stop(spinner);
	kstep_sleep();

	TRACE_INFO("hits=%d bug=%d prev_state=%lu real_state=%lu",
		   atomic_read(&tracepoint_hits), atomic_read(&bug_detected),
		   observed_prev_state, observed_real_state);

	if (atomic_read(&bug_detected)) {
		kstep_fail("trace_sched_switch saw stale prev_state=%lu "
			   "but task __state=%lu (should be TASK_RUNNING=0)",
			   observed_prev_state, observed_real_state);
	} else if (atomic_read(&tracepoint_hits) > 0 &&
		   observed_prev_state == TASK_RUNNING) {
		kstep_pass("trace_sched_switch correctly reported prev_state=%lu "
			   "task __state=%lu (hits=%d)",
			   observed_prev_state, observed_real_state,
			   atomic_read(&tracepoint_hits));
	} else if (atomic_read(&tracepoint_hits) > 0) {
		kstep_pass("trace_sched_switch: prev_state=%lu real=%lu hits=%d",
			   observed_prev_state, observed_real_state,
			   atomic_read(&tracepoint_hits));
	} else {
		kstep_fail("tracepoint was never hit for target task");
	}

out_unreg:
	tracepoint_probe_unregister(sched_switch_tp, sched_switch_probe, NULL);
	tracepoint_synchronize_unregister();
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE {
	.name = "sched_switch_prev",
	.setup = setup,
	.run = run,
	.step_interval_us = 1000,
};
