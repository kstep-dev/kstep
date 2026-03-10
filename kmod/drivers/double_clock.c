// https://github.com/torvalds/linux/commit/96500560f0c7
//
// Bug: __balance_push_cpu_stop() calls update_rq_clock() then __migrate_task()
// which also calls update_rq_clock(), creating a redundant double clock update.
// The fix removes update_rq_clock() from __migrate_task().
//
// Detection: __migrate_task() is inlined into __balance_push_cpu_stop(). We
// scan the compiled __balance_push_cpu_stop() for call instructions targeting
// update_rq_clock(). Buggy kernel: 2 calls (one explicit, one from inlined
// __migrate_task). Fixed kernel: 1 call (the explicit one only).

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 4, 0)

static void setup(void) {}

// Count direct calls (E8 rel32) to target within `size` bytes of fn.
static int count_calls_to(void *fn, void *target, int size) {
  unsigned char *code = (unsigned char *)fn;
  int count = 0;

  for (int i = 0; i < size - 5; i++) {
    if (code[i] == 0xe8) {
      int32_t rel = *(int32_t *)(code + i + 1);
      void *dest = (void *)((unsigned long)(code + i + 5) + (long)rel);
      if (dest == target)
        count++;
      i += 4;
    }
  }
  return count;
}

static void run(void) {
  void *fn_bps = kstep_ksym_lookup("__balance_push_cpu_stop");
  void *fn_urc = kstep_ksym_lookup("update_rq_clock");
  void *fn_next = kstep_ksym_lookup("push_cpu_stop");

  if (!fn_bps || !fn_urc) {
    kstep_fail("cannot look up symbols (bps=%p urc=%p)", fn_bps, fn_urc);
    return;
  }

  // Use the next symbol to determine function size, or default to 1024
  int fn_size = fn_next ? (int)((unsigned long)fn_next - (unsigned long)fn_bps)
                        : 1024;
  if (fn_size <= 0 || fn_size > 4096)
    fn_size = 1024;

  int calls = count_calls_to(fn_bps, fn_urc, fn_size);
  TRACE_INFO("update_rq_clock calls in __balance_push_cpu_stop: %d", calls);

  if (calls >= 2) {
    kstep_fail("double update_rq_clock in __balance_push_cpu_stop: "
               "found %d calls (inlined __migrate_task adds redundant call)",
               calls);
  } else if (calls == 1) {
    kstep_pass("single update_rq_clock in __balance_push_cpu_stop");
  } else {
    kstep_fail("unexpected: %d update_rq_clock calls found", calls);
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "double_clock",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
