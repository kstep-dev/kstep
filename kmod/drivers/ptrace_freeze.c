// Reproduce: sched: Fix race against ptrace_freeze_trace()
// https://github.com/torvalds/linux/commit/d136122f5845
//
// Bug: In __schedule(), prev->state is read BEFORE rq->lock, then re-read
// AFTER the lock for a double-check (prev_state == prev->state).
// ptrace_freeze_traced() can change state between reads under only siglock,
// causing the double-check to fail and skipping deactivate_task().
//
// Fix: Read prev->state once AFTER rq->lock; use control dependency.
//
// Strategy: A lock-holder kthread on CPU 2 holds rq_lock(1) to widen the
// race window. Target on CPU 1 enters __schedule(), reads prev->state,
// then spins on rq_lock. While spinning, we change state from CPU 0.
// On the buggy kernel, the double-check sees mismatched values and the
// kernel's race counter increments. On the fixed kernel, the single-read
// path has no double-check to fail.

#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

KSYM_IMPORT_TYPED(atomic_t, kstep_sched_race_count);

static struct task_struct *target;
static struct task_struct *locker_task;

// Coordination: 0=idle, 1=acquire-lock, 2=lock-held, 3=release-lock
static atomic_t phase;

static int locker_fn(void *data)
{
struct rq *rq1 = cpu_rq(1);

while (!kthread_should_stop()) {
if (atomic_read(&phase) != 1) {
cpu_relax();
continue;
}

raw_spin_lock_irq(&rq1->lock);
atomic_set(&phase, 2);

while (atomic_read(&phase) != 3)
cpu_relax();

raw_spin_unlock_irq(&rq1->lock);
atomic_set(&phase, 0);
}
return 0;
}

static void setup(void)
{
atomic_set(&phase, 0);

target = kstep_kthread_create("target");
kstep_kthread_bind(target, cpumask_of(1));
kstep_kthread_start(target);

locker_task = kthread_create(locker_fn, NULL, "locker");
kthread_bind(locker_task, 2);
wake_up_process(locker_task);
}

static void run(void)
{
TRACE_INFO("initial ticks to let target settle");
kstep_tick_repeat(3);

int race_before = atomic_read(KSYM_kstep_sched_race_count);
TRACE_INFO("race counter before: %d", race_before);

// Step 1: Tell locker to hold rq_lock(1)
atomic_set(&phase, 1);
while (atomic_read(&phase) != 2)
cpu_relax();
TRACE_INFO("rq_lock(1) held by locker on CPU 2");

// Step 2: Block target - enters wait_event -> schedule -> __schedule
kstep_kthread_block(target);

// Step 3: Wait for target to enter __schedule and spin on the lock
udelay(2000);

// Step 4: Flip target state to a different non-zero value
long old_state = READ_ONCE(target->state);
TRACE_INFO("target state before flip: 0x%lx on_cpu=%d", old_state,
   target->on_cpu);
if (old_state == TASK_UNINTERRUPTIBLE)
WRITE_ONCE(target->state, TASK_INTERRUPTIBLE);
else if (old_state != 0)
WRITE_ONCE(target->state, old_state ^ 3);

// Step 5: Release the lock
atomic_set(&phase, 3);

// Step 6: Let the scheduler finish
kstep_sleep();
kstep_tick_repeat(3);

int race_after = atomic_read(KSYM_kstep_sched_race_count);
int on_rq = READ_ONCE(target->on_rq);
long state = READ_ONCE(target->state);
TRACE_INFO("race counter after: %d (delta=%d)", race_after,
   race_after - race_before);
TRACE_INFO("result: on_rq=%d state=0x%lx", on_rq, state);

if (race_after > race_before) {
kstep_fail("BUG: double-check race detected %d time(s); "
   "deactivate_task was skipped",
   race_after - race_before);
} else {
kstep_pass("no double-check race detected");
}

kthread_stop(locker_task);
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
