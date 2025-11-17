#include <linux/kernel.h>
#include <linux/sched_clock.h>
#include <linux/seqlock.h>

#include "internal.h"
#include "ksym.h"
#include "logging.h"

static u64 clock_value = 0;
static u64 kstep_sched_clock(void) { return clock_value; }

#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
// On x86_64 with paravirt enabled, `sched_clock` (see `arch/x86/kernel/tsc.c`)
// is a wrapper of `paravirt_sched_clock` which can be changed with
// `paravirt_set_sched_clock` (see `arch/x86/include/asm/paravirt.h`).

static void kstep_sched_clock_init(void) {
  *ksym.__sched_clock_offset = 0;
  ksym.paravirt_set_sched_clock(kstep_sched_clock);
}

static void kstep_sched_clock_exit(void) {
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

static void kstep_sched_clock_init(void) {
  struct clock_data *cd = ksym.cd;
  memcpy(&cd_backup, cd, sizeof(struct clock_data));
  cd->actual_read_sched_clock = kstep_sched_clock;
  for (int i = 0; i < 2; i++) {
    struct clock_read_data *rd = &cd->read_data[i];
    rd->read_sched_clock = kstep_sched_clock;
    rd->mult = 1;
    rd->shift = 0;
    rd->epoch_ns = 0;
    rd->epoch_cyc = 0;
  }
}

static void kstep_sched_clock_exit(void) {
  memcpy(ksym.cd, &cd_backup, sizeof(struct clock_data));
}

#else
#error "Sched clock mocking not supported for this platform"
#endif

static void kstep_jiffies_init(void) {
  // Avoid calling `tick_do_update_jiffies64` and `do_timer` to update jiffies.
  // They are called by `tick_sched_do_timer` and `tick_periodic` respectively,
  // and guarded by `tick_do_timer_cpu == cpu` to check if the current CPU is
  // the timekeeper CPU for updating jiffies.
  // References:
  // `tick_sched_do_timer`:https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-sched.c#L206
  // `tick_periodic`:https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-common.c#L86
  // `tick_do_timer_cpu`:https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-common.c#L51

  // Setting an invalid timerkeeper CPU to avoid (most of) jiffies updates.
  *ksym.tick_do_timer_cpu = -1;

  // Unfortunate workaround:
  // https://github.com/torvalds/linux/commit/a1ff03cd6fb9c501fff63a4a2bface9adcfa81cd
  // allows a non-timekeeper CPU to update jiffies. We force
  // `tick_do_update_jiffies64` to be a noop function to avoid the update.
  kstep_patch_func_noop("tick_do_update_jiffies64");
  TRACE_INFO("Disabled jiffies update");
}

static void kstep_jiffies_exit(void) {
  *ksym.tick_do_timer_cpu = 0;
  TRACE_INFO("Enabled jiffies update");
}

void kstep_clock_init(u64 value) {
  kstep_jiffies_init();
  kstep_sched_clock_init();

  clock_value = value;
  jiffies = INITIAL_JIFFIES + nsecs_to_jiffies(clock_value);
  smp_mb();
  TRACE_INFO("Initialized clock to %llu ns and jiffies to %lu", clock_value,
             (jiffies - INITIAL_JIFFIES));
}

void kstep_clock_tick(void) {
  clock_value += TICK_NSEC;
  jiffies += 1;
  smp_mb();
}

void kstep_clock_exit(void) {
  kstep_sched_clock_exit();
  kstep_jiffies_exit();
}
