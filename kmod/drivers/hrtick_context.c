// https://github.com/torvalds/linux/commit/e38e5299747b
//
// Bug: hrtick() calls rq->donor->sched_class->task_tick(rq, rq->curr, 1)
// passing rq->curr instead of rq->donor. When proxy execution causes
// donor != curr (e.g., RT donor with CFS curr), the wrong task is passed
// to the scheduling class's task_tick method.
//
// Fix: Change rq->curr to rq->donor in the hrtick() call.
//
// Observable: Set up proxy execution (RT donor, CFS curr), then directly
// arm the hrtick timer. When it fires, kprobe on task_tick_rt detects
// that a non-RT task was passed (the CFS curr instead of the RT donor).

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 18, 0)

#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/sched/rt.h>
#include <linux/kprobes.h>

static DEFINE_MUTEX(proxy_mutex);
static struct task_struct *cfs_thread;
static struct task_struct *rt_thread;
static atomic_t test_done = ATOMIC_INIT(0);
static atomic_t hrtick_fired = ATOMIC_INIT(0);
static atomic_t wrong_task_ticked = ATOMIC_INIT(0);
static int ticked_task_pid;
static int ticked_task_policy;
static int expected_donor_pid;

// Kprobe on task_tick_rt to detect if a non-RT task is passed via hrtick
// task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
// On x86_64: rdi=rq, rsi=p, edx=queued
// hrtick calls with queued=1, sched_tick calls with queued=0
static struct kprobe tick_rt_kp;

static int tick_rt_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct task_struct *task = (struct task_struct *)regs->si;
	int queued = (int)regs->dx;
	int cpu = smp_processor_id();

	if (cpu < 1 || queued != 1)
		return 0;

	// This is from hrtick (queued=1). Check if the task is actually RT.
	atomic_inc(&hrtick_fired);

	if (task->policy != SCHED_FIFO && task->policy != SCHED_RR) {
		struct rq *rq = cpu_rq(cpu);
		ticked_task_pid = task->pid;
		ticked_task_policy = task->policy;
		expected_donor_pid = rq->donor->pid;
		atomic_inc(&wrong_task_ticked);
		smp_mb();
		TRACE_INFO("HRTICK BUG: task_tick_rt called with non-RT task "
			   "pid=%d (policy=%d) via hrtick. donor=%d (policy=%d). "
			   "Should have passed donor, not curr.",
			   task->pid, task->policy,
			   rq->donor->pid, rq->donor->policy);
	}
	return 0;
}

// Arm the hrtick timer on the current CPU (called via IPI on CPU 1)
static void arm_hrtick_ipi(void *data)
{
	struct rq *rq = this_rq();

	TRACE_INFO("Arming hrtick on CPU %d: donor=%d curr=%d",
		   smp_processor_id(), rq->donor->pid, rq->curr->pid);

	// Arm the hrtick timer to fire very soon (100us)
	hrtimer_start(&rq->hrtick_timer, ns_to_ktime(100000),
		      HRTIMER_MODE_REL_PINNED_HARD);
}

// CFS thread: hold mutex and yield periodically until told to stop
static int cfs_thread_fn(void *data)
{
	mutex_lock(&proxy_mutex);
	TRACE_INFO("CFS thread %d acquired mutex", current->pid);
	while (!atomic_read(&test_done))
		cond_resched();
	mutex_unlock(&proxy_mutex);
	return 0;
}

// RT thread: try to acquire the mutex (will block, triggering proxy exec)
static int rt_thread_fn(void *data)
{
	TRACE_INFO("RT thread %d trying to acquire mutex", current->pid);
	mutex_lock(&proxy_mutex);
	TRACE_INFO("RT thread %d acquired mutex", current->pid);
	mutex_unlock(&proxy_mutex);
	return 0;
}

static void setup(void) {}

static void run(void)
{
	struct sched_param param = { .sched_priority = 50 };
	int ret;

	// Register kprobe on task_tick_rt to detect wrong task argument
	tick_rt_kp.symbol_name = "task_tick_rt";
	tick_rt_kp.pre_handler = tick_rt_pre_handler;
	ret = register_kprobe(&tick_rt_kp);
	if (ret < 0) {
		kstep_fail("Failed to register kprobe on task_tick_rt: %d", ret);
		return;
	}
	TRACE_INFO("Registered kprobe on task_tick_rt at %pS", tick_rt_kp.addr);

	// Create CFS thread on CPU 1
	cfs_thread = kthread_create(cfs_thread_fn, NULL, "hrtick_cfs");
	if (IS_ERR(cfs_thread)) {
		kstep_fail("Failed to create CFS thread");
		goto out_kprobe;
	}
	kthread_bind(cfs_thread, 1);
	wake_up_process(cfs_thread);

	// Let CFS thread run and acquire the mutex
	kstep_sleep();
	kstep_tick_repeat(5);
	kstep_sleep();

	TRACE_INFO("CFS thread pid=%d running on CPU 1", cfs_thread->pid);

	// Create RT thread on CPU 1
	rt_thread = kthread_create(rt_thread_fn, NULL, "hrtick_rt");
	if (IS_ERR(rt_thread)) {
		kstep_fail("Failed to create RT thread");
		goto out_cleanup;
	}
	kthread_bind(rt_thread, 1);
	{
		typedef int (*setscheduler_fn_t)(struct task_struct *, int,
						const struct sched_param *);
		setscheduler_fn_t fn = kstep_ksym_lookup("sched_setscheduler_nocheck");
		if (!fn)
			panic("Failed to find sched_setscheduler_nocheck");
		fn(rt_thread, SCHED_FIFO, &param);
	}
	wake_up_process(rt_thread);

	TRACE_INFO("RT thread pid=%d woken on CPU 1 (SCHED_FIFO prio=%d)",
		   rt_thread->pid, param.sched_priority);

	// Give RT thread time to get scheduled, call mutex_lock, and block.
	// cond_resched() in CFS thread allows scheduling when NEED_RESCHED is set.
	for (int i = 0; i < 5; i++) {
		kstep_tick();
		kstep_sleep();
	}

	// Verify proxy execution state
	{
		struct rq *rq1 = cpu_rq(1);
		TRACE_INFO("Proxy state: donor=%d (%s, policy=%d) "
			   "curr=%d (%s, policy=%d)",
			   rq1->donor->pid, rq1->donor->comm, rq1->donor->policy,
			   rq1->curr->pid, rq1->curr->comm, rq1->curr->policy);

		if (rq1->donor == rq1->curr) {
			kstep_fail("proxy execution not active (donor==curr)");
			goto out_cleanup;
		}
	}

	// Directly arm the hrtick timer on CPU 1 via IPI
	smp_call_function_single(1, arm_hrtick_ipi, NULL, 1);

	// Sleep to let the hrtick timer fire
	kstep_sleep();
	kstep_sleep();
	smp_mb();

	{
		int fired = atomic_read(&hrtick_fired);
		int wrong = atomic_read(&wrong_task_ticked);
		TRACE_INFO("hrtick_fired=%d wrong_task_ticked=%d", fired, wrong);

		if (wrong > 0) {
			kstep_fail("hrtick passed wrong task to task_tick: "
				   "ticked curr pid=%d (policy=%d) but donor "
				   "was pid=%d (RT) -- should tick donor not curr",
				   ticked_task_pid, ticked_task_policy,
				   expected_donor_pid);
		} else if (fired > 0) {
			kstep_pass("hrtick fired and passed correct task "
				   "(donor) to task_tick (fired=%d)", fired);
		} else {
			kstep_fail("hrtick timer never fired");
		}
	}

out_cleanup:
	atomic_set(&test_done, 1);
	kstep_sleep();

out_kprobe:
	unregister_kprobe(&tick_rt_kp);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE {
	.name = "hrtick_context",
	.setup = setup,
	.run = run,
	.step_interval_us = 1000,
};
