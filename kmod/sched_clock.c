#include <linux/sched_clock.h>
#include <linux/seqlock.h>

#include "internal.h"
#include "ksym.h"

static u64 clock_value = 0;
static u64 sched_clock(void) { return clock_value; }

void sched_clock_set(u64 value) { clock_value = value; }
void sched_clock_inc(u64 delta) { clock_value += delta; }

#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
// On x86_64 with paravirt enabled, `sched_clock` (see `arch/x86/kernel/tsc.c`)
// is a wrapper of `paravirt_sched_clock` which can be changed with
// `paravirt_set_sched_clock` (see `arch/x86/include/asm/paravirt.h`).

void sched_clock_init(void) {
  *ksym.__sched_clock_offset = 0;
  ksym.paravirt_set_sched_clock(sched_clock);
}

void sched_clock_exit(void) {
  ksym.paravirt_set_sched_clock(ksym.kvm_sched_clock_read);
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

static struct clock_data cd_backup;

void sched_clock_init(void) {
  struct clock_data *cd = ksym.cd;
  memcpy(&cd_backup, cd, sizeof(struct clock_data));
  cd->actual_read_sched_clock = sched_clock;
  for (int i = 0; i < 2; i++) {
    struct clock_read_data *rd = &cd->read_data[i];
    rd->read_sched_clock = sched_clock;
    rd->mult = 1;
    rd->shift = 0;
    rd->epoch_ns = 0;
    rd->epoch_cyc = 0;
  }
}

void sched_clock_exit(void) {
  memcpy(ksym.cd, &cd_backup, sizeof(struct clock_data));
}

#else
#error "Sched clock mocking not supported for this platform"
#endif
