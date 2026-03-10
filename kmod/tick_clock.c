#include <linux/sched/clock.h>
#include <linux/sched_clock.h>
#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
#include <asm/timer.h>
#include <asm/paravirt.h>
#endif

#include "internal.h"

static u64 kstep_sched_clock = INIT_TIME_NS;
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
  *KSYM___sched_clock_offset = 0;
  // paravirt_set_sched_clock was added mid-cycle in v5.12; use runtime
  // lookup to handle kernels where the symbol does not yet exist.
  typedef void (*set_sc_fn)(u64 (*)(void));
  set_sc_fn set_fn = (set_sc_fn)kstep_ksym_lookup("paravirt_set_sched_clock");
  if (set_fn) {
    set_fn(kstep_sched_clock_get);
  } else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
    // Fallback for kernels < 5.12: directly patch pv_ops.time.sched_clock
    KSYM_IMPORT(pv_ops);
    KSYM_pv_ops->time.sched_clock =
        (unsigned long long (*)(void))kstep_sched_clock_get;
#else
    panic("paravirt_set_sched_clock not found and pv_ops fallback not available");
#endif
  }
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
