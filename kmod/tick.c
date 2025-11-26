#include "kstep.h"

#include <linux/delay.h>
#include <linux/sched_clock.h>

static u64 kstep_clock_value = INIT_TIME_NS;
static u64 kstep_sched_clock(void) { return kstep_clock_value; }
static u64 kstep_jiffies(void) {
  return INITIAL_JIFFIES + nsecs_to_jiffies(kstep_clock_value);
}

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

  // It's possible that a non-timekeeper CPU calls `tick_do_update_jiffies64`
  // https://github.com/torvalds/linux/commit/a1ff03cd6fb9c501fff63a4a2bface9adcfa81cd
  // We set `tick_next_period` to a large value to avoid such updates within
  // `tick_do_update_jiffies64`
  *ksym.tick_next_period = KTIME_MAX;

  TRACE_INFO("Disabled jiffies update");
}

static void kstep_jiffies_exit(void) {
  *ksym.tick_do_timer_cpu = 0;
  *ksym.tick_next_period = 0;
  TRACE_INFO("Enabled jiffies update");
}

static void kstep_sched_timer_init(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    // Ref: tick_sched_timer_dying in
    // https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-sched.c#L1606
    struct tick_sched *ts = ksym.tick_get_tick_sched(cpu);
    hrtimer_cancel(&ts->sched_timer);
    memset(ts, 0, sizeof(struct tick_sched));
    TRACE_INFO("Disabled timer ticks on CPU %d", cpu);
  }
}

static void kstep_sched_timer_exit(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.tick_setup_sched_timer,
                             (void *)true, 0);
  }
}

void kstep_tick_init(void) {
  kstep_sched_timer_init();
  kstep_jiffies_init();
  kstep_sched_clock_init();

  jiffies = kstep_jiffies();
  smp_mb();

  TRACE_INFO("Initialized clock to %llu ns and jiffies to %lu",
             kstep_clock_value, (jiffies - INITIAL_JIFFIES));
}

void kstep_tick_exit(void) {
  kstep_sched_clock_exit();
  kstep_jiffies_exit();
  kstep_sched_timer_exit();
}

void kstep_sleep(void) { udelay(kstep_params.step_interval_us); }

void kstep_tick(void) {
  if (kstep_params.print_rq_stats)
    print_rq_stats();
  if (kstep_params.print_tasks)
    print_tasks();
  if (kstep_params.print_nr_running)
    print_nr_running();

  if (jiffies != kstep_jiffies()) {
    panic("Jiffies mismatch: %lu != %llu", jiffies, kstep_jiffies());
  }

  kstep_clock_value += TICK_NSEC;
  jiffies = kstep_jiffies();
  smp_mb();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  void *sched_tick = (void *)ksym.sched_tick;
#else
  void *sched_tick = (void *)ksym.scheduler_tick;
#endif
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, sched_tick, NULL, 0);
    kstep_sleep();
  }
}

void kstep_tick_until(bool (*fn)(void)) {
  while (1) {
    if (fn())
      return;
    kstep_tick();
  }
}

struct task_struct *kstep_tick_until_task(bool (*fn)(struct task_struct *)) {
  struct task_struct *p;
  while (1) {
    for_each_process(p) {
      if (fn(p))
        return p;
    }
    kstep_tick();
  }
}
