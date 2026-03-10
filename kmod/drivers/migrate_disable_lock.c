// https://github.com/torvalds/linux/commit/942b8db96500
//
// Bug: migrate_disable_switch() calls __do_set_cpus_allowed() without
// holding p->pi_lock, violating the affinity locking protocol.
// The fix moves the call before rq->lock acquisition and wraps it
// with proper task_rq_lock guards.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 18, 0)

static struct task_struct *filler;

static void setup(void) {
  filler = kstep_task_create();
}

static void run(void) {
  extern atomic_t kstep_migrate_disable_pi_locked;
  atomic_set(&kstep_migrate_disable_pi_locked, -1);

  // Pin filler task to CPU 1 so there's a competing runnable task
  kstep_task_pin(filler, 1, 1);

  // Create a kthread on CPU 1 that will spin
  struct task_struct *kt = kstep_kthread_create("kstep_mig");
  kstep_kthread_bind(kt, cpumask_of(1));
  kstep_kthread_start(kt);

  // Let both tasks get scheduled on CPU 1
  kstep_tick_repeat(10);

  // Set migration_disabled on the kthread (normally done by migrate_disable())
  // This is safe because the field is only checked during context switch.
  WRITE_ONCE(kt->migration_disabled, 1);
  smp_mb();

  TRACE_INFO("kt pid=%d migration_disabled=%d cpus_ptr==cpus_mask=%d",
             kt->pid, kt->migration_disabled,
             kt->cpus_ptr == &kt->cpus_mask);

  // Make the kthread yield, triggering __schedule() → migrate_disable_switch()
  kstep_kthread_yield(kt);

  // Tick to let the yield and context switch happen
  kstep_tick_repeat(20);

  int val = atomic_read(&kstep_migrate_disable_pi_locked);
  TRACE_INFO("migrate_disable pi_locked=%d", val);

  if (val == 0) {
    kstep_fail("pi_lock NOT held during migrate_disable_switch - locking bug");
  } else if (val == 1) {
    kstep_pass("pi_lock held during migrate_disable_switch");
  } else {
    kstep_fail("migrate_disable_switch not triggered (val=%d)", val);
  }
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "migrate_disable_lock",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
