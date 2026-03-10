// Reproduce: sched: Fix race against ptrace_freeze_trace()
// https://github.com/torvalds/linux/commit/d136122f5845
//
// Bug: __schedule() reads prev->state BEFORE rq->lock (READ 1), then
// double-checks (prev_state == prev->state) AFTER. ptrace_freeze_traced()
// can change state between reads, failing the double-check and skipping
// deactivate_task(). Task stays on_rq with a sleeping state.
//
// Fix: Read prev->state once AFTER rq->lock; use control dependency.
//
// Strategy: A kprobe on update_rq_clock toggles the target's sleeping state
// each time __schedule runs on CPU 1. On the buggy kernel, the double-check
// fails repeatedly (many toggles). On the fixed kernel, the single read
// after the kprobe sees non-zero state and deactivates immediately.

#include "driver.h"
#include "internal.h"
#include <linux/kprobes.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

static struct task_struct *target;
static volatile bool armed;
static struct kprobe kp;
static volatile int toggle_count;

static int kp_pre(struct kprobe *p, struct pt_regs *regs)
{
if (!armed || smp_processor_id() != 1)
return 0;

struct rq *rq = cpu_rq(1);
if (rq->curr != target)
return 0;

long s = READ_ONCE(target->state);
if (s == TASK_INTERRUPTIBLE) {
WRITE_ONCE(target->state, TASK_UNINTERRUPTIBLE);
toggle_count++;
} else if (s == TASK_UNINTERRUPTIBLE) {
WRITE_ONCE(target->state, TASK_INTERRUPTIBLE);
toggle_count++;
}
if (toggle_count >= 10)
armed = false;
return 0;
}

static void setup(void)
{
target = kstep_kthread_create("target");
kstep_kthread_bind(target, cpumask_of(1));
kstep_kthread_start(target);
}

static void run(void)
{
kstep_tick_repeat(5);

// Block target: it will call wait_event -> schedule()
kstep_kthread_block(target);

// Register kprobe now (after initial ticks to avoid interference)
kp.symbol_name = "update_rq_clock";
kp.pre_handler = kp_pre;
int ret = register_kprobe(&kp);
if (ret < 0) {
kstep_fail("register_kprobe failed: %d", ret);
return;
}
TRACE_INFO("kprobe registered, arming");

// Arm and tick
armed = true;
kstep_tick_repeat(10);
armed = false;

int tc = toggle_count;
int on_rq = READ_ONCE(target->on_rq);
long state = READ_ONCE(target->state);

unregister_kprobe(&kp);

TRACE_INFO("result: toggle_count=%d on_rq=%d state=0x%lx",
   tc, on_rq, state);

// Buggy: many toggles (double-check kept failing, blocking deactivation)
// Fixed: few toggles (deactivation succeeded on first schedule)
if (tc >= 5) {
kstep_fail("ptrace race: toggle_count=%d "
   "(double-check prevented deactivation)", tc);
} else {
kstep_pass("task deactivated correctly (toggle_count=%d)", tc);
}
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "ptrace_freeze",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
