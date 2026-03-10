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

	// Only check on the schedule() triggered by our TASK_INTERRUPTIBLE path,
	// not on the kthread exit path (TASK_DEAD=0x80)
	if (prev_state == TASK_INTERRUPTIBLE || prev_state == TASK_RUNNING) {
		observed_prev_state = prev_state;
		observed_real_state = READ_ONCE(prev->__state);
		// Bug: prev_state indicates sleeping but task is actually TASK_RUNNING
		if (prev_state != 0 && observed_real_state == TASK_RUNNING)
			atomic_set(&bug_detected, 1);
	}
}

static void find_sched_switch_tp(struct tracepoint *tp, void *priv)
{
	if (!strcmp(tp->name, "sched_switch"))
		sched_switch_tp = tp;
}

// Kthread that just spins - provides a context switch target on CPU 1
static int spinner_fn(void *data)
{
	while (!kthread_should_stop())
		cpu_relax();
	return 0;
}

// Kthread function: set TASK_INTERRUPTIBLE, set TIF_SIGPENDING, schedule()
// Needs another runnable task on the same CPU so that schedule() actually
// switches to a different task (triggering trace_sched_switch).
static int test_thread_fn(void *data)
{
	// Set ourselves to TASK_INTERRUPTIBLE
	set_current_state(TASK_INTERRUPTIBLE);

	// Set TIF_SIGPENDING so signal_pending_state() returns true
	set_tsk_thread_flag(current, TIF_SIGPENDING);

	// Call schedule(). In __schedule():
	//   prev_state = TASK_INTERRUPTIBLE
	//   try_to_block_task detects signal -> sets __state = TASK_RUNNING
	//   But prev_state is NOT updated (buggy kernel)
	//   trace_sched_switch sees stale prev_state
	schedule();

	// Clear the flag so we can exit cleanly
	clear_tsk_thread_flag(current, TIF_SIGPENDING);
	return 0;
}

static void setup(void) {}

static void run(void)
{
	struct task_struct *spinner;
	int ret;

	// Find sched_switch tracepoint
	for_each_kernel_tracepoint(find_sched_switch_tp, NULL);
	if (!sched_switch_tp) {
		kstep_fail("Could not find sched_switch tracepoint");
		return;
	}

	// Register probe
	ret = tracepoint_probe_register(sched_switch_tp, sched_switch_probe, NULL);
	if (ret) {
		kstep_fail("Failed to register sched_switch probe: %d", ret);
		return;
	}
	TRACE_INFO("Registered sched_switch tracepoint probe");

	// Create a spinner kthread on CPU 1 to ensure context switches happen
	spinner = kthread_create(spinner_fn, NULL, "sswitch_spin");
	if (IS_ERR(spinner)) {
		kstep_fail("Failed to create spinner kthread");
		goto out_unreg;
	}
	kthread_bind(spinner, 1);
	wake_up_process(spinner);
	TRACE_INFO("Started spinner kthread pid=%d on CPU 1", spinner->pid);

	// Let spinner get scheduled
	kstep_sleep();
	kstep_tick_repeat(3);
	kstep_sleep();

	// Create test kthread on CPU 1
	target = kthread_create(test_thread_fn, NULL, "sswitch_test");
	if (IS_ERR(target)) {
		kstep_fail("Failed to create test kthread: %ld", PTR_ERR(target));
		kthread_stop(spinner);
		goto out_unreg;
	}
	kthread_bind(target, 1);
	wake_up_process(target);
	TRACE_INFO("Started test kthread pid=%d on CPU 1", target->pid);

	// Let the test kthread run and trigger the bug
	kstep_sleep();
	kstep_sleep();
	kstep_tick_repeat(5);
	kstep_sleep();
	kstep_sleep();

	// Stop the spinner
	kthread_stop(spinner);
	kstep_sleep();

	// Report results
	TRACE_INFO("tracepoint_hits=%d bug_detected=%d",
		   atomic_read(&tracepoint_hits), atomic_read(&bug_detected));

	if (atomic_read(&bug_detected)) {
		kstep_fail("trace_sched_switch saw stale prev_state=%lu "
			   "but task __state=%lu (should be TASK_RUNNING=0)",
			   observed_prev_state, observed_real_state);
	} else if (atomic_read(&tracepoint_hits) > 0) {
		kstep_pass("trace_sched_switch correctly reported prev_state=%lu "
			   "task __state=%lu (hits=%d)",
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
