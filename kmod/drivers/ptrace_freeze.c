// Reproduce: sched: Fix race against ptrace_freeze_trace()
// https://github.com/torvalds/linux/commit/d136122f5845
//
// Bug: In __schedule(), prev->state is read BEFORE rq->lock (READ 1), then
// double-checked AFTER the lock (prev_state == prev->state). If the state
// changes between reads, the double-check fails and deactivate_task() is
// skipped. The task stays on_rq with a sleeping state.
//
// Fix: Read prev->state once AFTER rq->lock; use control dependency.
//
// Strategy: A kprobe on update_rq_clock fires between READ 1 and the
// double-check on the buggy kernel. It toggles the target's state between
// TASK_INTERRUPTIBLE and TASK_UNINTERRUPTIBLE each time. On the buggy
// kernel, this creates an infinite loop where the task is never deactivated
// (toggle_count grows). On the fixed kernel, the single read sees the
// toggled (still non-zero) state and deactivates correctly (toggle_count=1).

#include "driver.h"
#include "internal.h"
#include <linux/kprobes.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

static struct task_struct *target;
static volatile bool armed;
static struct kprobe kp;
static int toggle_count;

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

kp.symbol_name = "update_rq_clock";
kp.pre_handler = kp_pre;
int ret = register_kprobe(&kp);
if (ret < 0)
pr_err("ptrace_freeze: register_kprobe failed: %d\n", ret);
else
TRACE_INFO("kprobe on update_rq_clock registered");
}

static void run(void)
{
kstep_tick_repeat(5);

// Block target: enters wait_event -> set_current_state(TASK_INTERRUPTIBLE)
// -> schedule() -> __schedule(preempt=false)
kstep_kthread_block(target);

// Arm kprobe to toggle state during __schedule
armed = true;

// Tick to let target call schedule(); kprobe toggles state in the window
kstep_tick_repeat(10);

armed = false;
unregister_kprobe(&kp);

TRACE_INFO("result: toggle_count=%d on_rq=%d state=0x%lx",
   toggle_count, target->on_rq, target->state);

// On buggy kernel: many toggles (double-check kept failing)
// On fixed kernel: 1-2 toggles (deactivation succeeded immediately)
if (toggle_count >= 5) {
kstep_fail("ptrace race: toggle_count=%d "
   "(double-check prevented deactivation repeatedly)",
   toggle_count);
} else {
kstep_pass("task deactivated correctly (toggle_count=%d)",
   toggle_count);
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
