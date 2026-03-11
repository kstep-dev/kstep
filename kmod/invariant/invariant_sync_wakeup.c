// Invariant for sync wakeup locality.
//
// Capture (at try_to_wake_up entry):
//   Records the waker CPU and whether the precondition is met:
//   1. Wake flags include WF_SYNC.
//   2. Wakee is in the CFS scheduling class.
//   3. Wakee's prev_cpu and waker's CPU do NOT share cache.
//
// Assertion (at try_to_wake_up return):
//   If precondition was met, waker's CPU must have at least one runnable task
//   (nr_running >= 1).

#include <linux/version.h>

#include <asm/ptrace.h>
#include <linux/ftrace.h>
#include "internal.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
struct kstep_invariant invariant_sync_wakeup = {
  .name      = "sync_wakeup_locality",
  .func_name = "try_to_wake_up",
  .per_cpu   = false,
  .capture   = NULL,
  .verify    = NULL,
};

#else

// try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags)
#define TTWU_WAKEE(fregs)      ((struct task_struct *) ftrace_regs_get_argument(fregs, 0))
#define TTWU_WAKE_FLAGS(fregs) ((int)                  ftrace_regs_get_argument(fregs, 2))

struct sync_wakeup_state {
  bool apply;      /* precondition met — verify should run */
  int  waker_cpu;
};

static void capture(const struct kstep_inv_ctx *ctx, void *out) {
  struct sync_wakeup_state *s = out;
  struct task_struct *wakee = TTWU_WAKEE(ctx->fregs);
  int wake_flags            = TTWU_WAKE_FLAGS(ctx->fregs);

  KSYM_IMPORT(fair_sched_class);

  s->waker_cpu = ctx->cpu;
  s->apply = (wake_flags & WF_SYNC) &&
             wakee->sched_class == KSYM_fair_sched_class;
}

static bool verify(const struct kstep_inv_ctx *ctx, const void *invariant_state) {
  struct sync_wakeup_state *s = (struct sync_wakeup_state *) invariant_state;
  if (!s->apply)
    return true;

  // Use the low-level API to get the number of running tasks on the waker CPU
  // instead of aggregated stats like nr_running, h_nr_queued, h_nr_runnable, etc.
  struct task_struct *p;
  int nr = 0;
  for_each_process(p) {
    if (task_cpu(p) == s->waker_cpu && p->__state == TASK_RUNNING)
      nr++;
  }

  if (nr <= 1)
    return false;

  return true;
}

struct kstep_invariant invariant_sync_wakeup = {
  .name      = "sync_wakeup_locality",
  .func_name = "try_to_wake_up",
  .per_cpu   = false,
  .capture   = capture,
  .verify    = verify,
};

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0) */
