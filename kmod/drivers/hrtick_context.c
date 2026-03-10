// https://github.com/torvalds/linux/commit/e38e5299747b
//
// Bug: hrtick() calls rq->donor->sched_class->task_tick(rq, rq->curr, 1)
// passing rq->curr instead of rq->donor. When proxy execution causes
// donor != curr (e.g., RT donor with CFS curr), the wrong task is passed
// to the scheduling class's task_tick method.
//
// Fix: Change rq->curr to rq->donor in the hrtick() call.
//
// Observable: Register a kprobe on hrtick(). When it fires with
// donor != curr, the buggy kernel passes curr (wrong task) to task_tick.
// We detect this by checking if donor != curr when hrtick fires.

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
static atomic_t hrtick_mismatch = ATOMIC_INIT(0);
static int hrtick_donor_pid;
static int hrtick_curr_pid;

// Kprobe on hrtick to detect donor/curr mismatch
static struct kprobe hrtick_kp;

static int hrtick_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	int cpu = smp_processor_id();
	struct rq *rq;

	if (cpu < 1)
		return 0;

	rq = cpu_rq(cpu);
	if (rq->donor != rq->curr) {
		hrtick_donor_pid = rq->donor->pid;
		hrtick_curr_pid = rq->curr->pid;
		atomic_inc(&hrtick_mismatch);
		TRACE_INFO("HRTICK donor/curr MISMATCH on CPU %d: "
			   "donor=%d (%s, policy=%d) curr=%d (%s, policy=%d)",
			   cpu,
			   rq->donor->pid, rq->donor->comm, rq->donor->policy,
			   rq->curr->pid, rq->curr->comm, rq->curr->policy);
	}
	return 0;
}

// CFS thread: hold mutex and spin until told to stop
static int cfs_thread_fn(void *data)
{
	mutex_lock(&proxy_mutex);
	TRACE_INFO("CFS thread %d acquired mutex", current->pid);
	while (!atomic_read(&test_done))
		cpu_relax();
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

	// Register kprobe on hrtick
	hrtick_kp.symbol_name = "hrtick";
	hrtick_kp.pre_handler = hrtick_pre_handler;
	ret = register_kprobe(&hrtick_kp);
	if (ret < 0) {
		kstep_fail("Failed to register kprobe on hrtick: %d", ret);
		return;
	}
	TRACE_INFO("Registered kprobe on hrtick at %pS", hrtick_kp.addr);

	// Enable hrtick via sched_features bitmask
	{
		unsigned int *feat = kstep_ksym_lookup("sysctl_sched_features");
		if (!feat)
			panic("Failed to find sysctl_sched_features");
		*feat |= (1U << __SCHED_FEAT_HRTICK);
		TRACE_INFO("Enabled HRTICK sched feature (features=0x%x)", *feat);
	}

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

	// RT thread should block on the mutex, causing proxy execution:
	//   donor = RT thread (highest prio, but blocked)
	//   curr = CFS thread (mutex owner, actually running)
	kstep_sleep();

	// Check rq state
	{
		struct rq *rq1 = cpu_rq(1);
		TRACE_INFO("After RT block: rq->donor=%d (%s) rq->curr=%d (%s)",
			   rq1->donor->pid, rq1->donor->comm,
			   rq1->curr->pid, rq1->curr->comm);
	}

	// Fire many ticks to trigger hrtick timer
	// sched_tick -> task_tick_fair -> hrtick_start_fair -> hrtick timer fires
	kstep_tick_repeat(50);
	kstep_sleep();
	kstep_sleep();

	// More ticks with sleeps to let hrtimers fire
	for (int i = 0; i < 10; i++) {
		kstep_tick_repeat(5);
		kstep_sleep();
	}

	TRACE_INFO("hrtick_mismatch count: %d", atomic_read(&hrtick_mismatch));

	if (atomic_read(&hrtick_mismatch) > 0) {
		kstep_fail("hrtick() fired with donor(%d) != curr(%d): "
			   "buggy kernel passes curr to task_tick instead of donor",
			   hrtick_donor_pid, hrtick_curr_pid);
	} else {
		// Check if proxy execution was even set up
		struct rq *rq1 = cpu_rq(1);
		if (rq1->donor == rq1->curr) {
			kstep_fail("proxy execution did not activate (donor==curr)");
		} else {
			kstep_pass("hrtick fired but donor==curr during all firings");
		}
	}

out_cleanup:
	atomic_set(&test_done, 1);
	kstep_sleep();
	kstep_tick_repeat(10);
	kstep_sleep();

out_kprobe:
	unregister_kprobe(&hrtick_kp);
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
