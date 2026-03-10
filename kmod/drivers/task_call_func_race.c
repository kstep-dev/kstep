// Reproduce: sched: Fix race in task_call_func()
// Commit: 91dabf33ae5df271da63e87ad7833e5fdb4a44b9
//
// The bug: task_call_func() checks only on_rq to decide if rq_lock is needed,
// but fails to synchronize with context switch completion (on_cpu). Between
// deactivate_task() setting on_rq=0 and context_switch() clearing on_cpu,
// task_call_func() can run its callback without rq_lock while the task is
// still executing on a CPU.
//
// Detection: Set on_cpu=1 on a sleeping task (simulating the race window),
// then call task_call_func(). On buggy kernel, callback sees on_cpu=1
// (race condition). On fixed kernel, task_call_func() waits for on_cpu=0
// via smp_cond_load_acquire before calling callback.

#include "internal.h"
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/wait.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 1, 0)

KSYM_IMPORT(task_call_func);

static struct task_struct *victim;
static int race_detected;

static int detect_race_cb(struct task_struct *p, void *arg)
{
  if (READ_ONCE(p->on_cpu) && !p->on_rq) {
    race_detected = 1;
    TRACE_INFO("RACE: pid=%d on_cpu=%d on_rq=%d state=%u",
               p->pid, p->on_cpu, p->on_rq, READ_ONCE(p->__state));
  }
  return 0;
}

static void clear_on_cpu_remote(void *info)
{
  struct task_struct *p = info;
  udelay(20);
  WRITE_ONCE(p->on_cpu, 0);
}

static void setup(void)
{
  victim = kstep_task_create();
  kstep_task_pin(victim, 1, 1);
}

static void run(void)
{
  kstep_task_wakeup(victim);
  kstep_tick_repeat(3);

  // Put the task to sleep indefinitely
  kstep_task_pause(victim);
  kstep_sleep();
  kstep_sleep();

  TRACE_INFO("Before manipulation: on_rq=%d on_cpu=%d state=%u",
             victim->on_rq, victim->on_cpu, READ_ONCE(victim->__state));

  if (victim->on_rq != 0 || victim->on_cpu != 0) {
    kstep_fail("Task not properly sleeping: on_rq=%d on_cpu=%d",
               victim->on_rq, victim->on_cpu);
    return;
  }

  // Simulate the race window: task dequeued (on_rq=0) but still on CPU
  WRITE_ONCE(victim->on_cpu, 1);

  // Queue IPI to CPU2: clears on_cpu after 20us.
  // On buggy kernel: task_call_func returns before IPI fires (no wait on on_cpu).
  // On fixed kernel: smp_cond_load_acquire spins until IPI clears on_cpu.
  smp_call_function_single(2, clear_on_cpu_remote, victim, 0);

  race_detected = 0;
  KSYM_task_call_func(victim, detect_race_cb, NULL);

  // Ensure on_cpu is restored
  WRITE_ONCE(victim->on_cpu, 0);

  if (race_detected)
    kstep_pass("task_call_func race: callback saw on_cpu=1 without rq_lock");
  else
    kstep_fail("task_call_func race: callback correctly saw on_cpu=0 (fix active)");

  kstep_tick_repeat(3);
}

KSTEP_DRIVER_DEFINE{
    .name = "task_call_func_race",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#endif
