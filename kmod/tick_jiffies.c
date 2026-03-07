#include "internal.h"

static u64 kstep_jiffies = 0;
static u64 kstep_jiffies_offset = 0;

u64 kstep_jiffies_get(void) { return kstep_jiffies; }

static void kstep_jiffies_set(u64 value) {
  kstep_jiffies = value;
  jiffies = kstep_jiffies + kstep_jiffies_offset;
  smp_mb();
}

void kstep_jiffies_init(void) {
  // Avoid calling `tick_do_update_jiffies64` and `do_timer` to update jiffies.
  // They are called by `tick_sched_do_timer` and `tick_periodic` respectively,
  // and guarded by `tick_do_timer_cpu == cpu` to check if the current CPU is
  // the timekeeper CPU for updating jiffies.
  // References:
  // `tick_sched_do_timer`:https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-sched.c#L206
  // `tick_periodic`:https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-common.c#L86
  // `tick_do_timer_cpu`:https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-common.c#L51

  // Setting an invalid timerkeeper CPU to avoid (most of) jiffies updates.
  KSYM_IMPORT(tick_do_timer_cpu);
  *KSYM_tick_do_timer_cpu = -1;

  // It's possible that a non-timekeeper CPU calls `tick_do_update_jiffies64`
  // https://github.com/torvalds/linux/commit/a1ff03cd6fb9c501fff63a4a2bface9adcfa81cd
  // We set `tick_next_period` to a large value to avoid such updates within
  // `tick_do_update_jiffies64`
  KSYM_IMPORT(tick_next_period);
  *KSYM_tick_next_period = KTIME_MAX;

  kstep_jiffies_offset = nsecs_to_jiffies(INIT_TIME_NS) + INITIAL_JIFFIES;
  kstep_jiffies_set(0);

  TRACE_INFO("Disabled jiffies update");
}

void kstep_jiffies_tick(void) {
  if (jiffies != kstep_jiffies + kstep_jiffies_offset)
    panic("%lu != %llu", jiffies, kstep_jiffies + kstep_jiffies_offset);
  kstep_jiffies_set(kstep_jiffies + 1);
}
