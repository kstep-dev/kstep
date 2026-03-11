#ifndef KSTEP_INVARIANT_H
#define KSTEP_INVARIANT_H

#include <linux/types.h>

struct ftrace_regs;
struct kstep_inv_ctx {
  int                cpu;
  struct ftrace_regs *fregs;  /* non-NULL only at INV_FUNC_ENTRY */
};

#define KSTEP_INV_MAX_STATE 256
#define KSTEP_INV_MAX_CPUS   16

struct kstep_invariant {
  const char *name;
  const char *func_name;
  bool per_cpu;
  void (*capture)(const struct kstep_inv_ctx *ctx, void *out); // capture the state of the invariant at the beginning of the function
  bool (*verify)(const struct kstep_inv_ctx *ctx, const void *invariant_state); // verify the invariant at the end of the function
  /* runtime state */
  u8   saved_state[KSTEP_INV_MAX_CPUS][KSTEP_INV_MAX_STATE];
};

// TODO: implement the invariant requiring time series data
// struct kstep_invariant_timeseries {
// }

extern struct kstep_invariant invariant_sync_wakeup;

#endif
