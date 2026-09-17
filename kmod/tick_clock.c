#include <linux/sched/clock.h>
#include <linux/sched_clock.h>
#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
#include <asm/timer.h>
#endif

#include "internal.h"

// The mocked clock starts one tick in; reset.c stamps the rq clocks and jiffies to match. One
// tick is the smallest epoch that keeps task_hot() honest (it reads rq_clock_task() - exec_start,
// which resets to 0, against sysctl_sched_migration_cost) and lands on a whole jiffy.
static u64 kstep_sched_clock = TICK_NSEC;
u64 kstep_sched_clock_get(void) { return kstep_sched_clock; }
void kstep_sched_clock_tick(void) {
  u64 interval = kstep_driver->tick_interval_ns ?: TICK_NSEC;
  kstep_sched_clock += interval;
}

#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
// On x86_64 with paravirt enabled, `sched_clock` (see `arch/x86/kernel/tsc.c`)
// is a wrapper of `paravirt_sched_clock` which can be changed with
// `paravirt_set_sched_clock` (see `arch/x86/include/asm/paravirt.h`).

void kstep_sched_clock_init(void) {
  KSYM_IMPORT(__sched_clock_offset);
  KSYM_IMPORT(paravirt_set_sched_clock);
  *KSYM___sched_clock_offset = 0;
  KSYM_paravirt_set_sched_clock(kstep_sched_clock_get);
  TRACE_INFO("Mocked sched clock");
}

#elif defined(CONFIG_GENERIC_SCHED_CLOCK)
// On other platforms (e.g., arm64), `sched_clock` is implemented in
// `kernel/time/sched_clock.c`, and we can change the function pointer in
// `struct clock_data` and `struct clock_read_data` to mock the sched clock.

struct clock_data {
  seqcount_latch_t seq;
  struct clock_read_data read_data[2];
  ktime_t wrap_kt;
  unsigned long rate;
  u64 (*actual_read_sched_clock)(void);
};

void kstep_sched_clock_init(void) {
  KSYM_IMPORT_TYPED(struct clock_data, cd);
  KSYM_cd->actual_read_sched_clock = kstep_sched_clock_get;
  for (int i = 0; i < 2; i++) {
    struct clock_read_data *rd = &KSYM_cd->read_data[i];
    rd->read_sched_clock = kstep_sched_clock_get;
    rd->mult = 1;
    rd->shift = 0;
    rd->epoch_ns = 0;
    rd->epoch_cyc = 0;
  }
  TRACE_INFO("Mocked sched clock");
}

#else
#error "Sched clock mocking not supported for this platform"
#endif
