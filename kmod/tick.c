#include <linux/delay.h>
#include <linux/sched_clock.h>

#include "internal.h"
#include <linux/kprobes.h>

static u64 kstep_sched_clock = INIT_TIME_NS;
static u64 kstep_sched_clock_read(void) { return kstep_sched_clock; }
static void kstep_sched_clock_tick(void) { kstep_sched_clock += TICK_NSEC; }

#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
// On x86_64 with paravirt enabled, `sched_clock` (see `arch/x86/kernel/tsc.c`)
// is a wrapper of `paravirt_sched_clock` which can be changed with
// `paravirt_set_sched_clock` (see `arch/x86/include/asm/paravirt.h`).

void kstep_sched_clock_init(void) {
  KSYM_IMPORT(__sched_clock_offset);
  KSYM_IMPORT(paravirt_set_sched_clock);
  *KSYM___sched_clock_offset = 0;
  KSYM_paravirt_set_sched_clock(kstep_sched_clock_read);
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
  KSYM_cd->actual_read_sched_clock = kstep_sched_clock_read;
  for (int i = 0; i < 2; i++) {
    struct clock_read_data *rd = &KSYM_cd->read_data[i];
    rd->read_sched_clock = kstep_sched_clock_read;
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

static u64 kstep_jiffies = 0;

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

  kstep_jiffies = nsecs_to_jiffies(kstep_sched_clock) + INITIAL_JIFFIES;
  jiffies = kstep_jiffies;
  smp_mb();

  TRACE_INFO("Disabled jiffies update");
}

static void kstep_jiffies_tick(void) {
  if (jiffies != kstep_jiffies)
    panic("%lu != %llu", jiffies, kstep_jiffies);
  kstep_jiffies++;
  jiffies = kstep_jiffies;
  smp_mb();
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
// kprobe tick_nohz_handler
static int tick_nohz_pre_handler(struct kprobe *kp, struct pt_regs *regs) {
  struct hrtimer *timer = (struct hrtimer *)regs->di;
  struct tick_sched *ts = container_of(timer, struct tick_sched, sched_timer);
  ts->flags |= 2; // Set TS_FLAG_STOPPED flag
  return 0;
}

static struct kprobe kstep_tick_nohz_kp = {
  .symbol_name = "tick_nohz_handler",
  .pre_handler = tick_nohz_pre_handler,
};
#endif

void kstep_sched_timer_init(void) {
  KSYM_IMPORT(tick_get_tick_sched);
  // Register kprobe for tick_nohz_handler to set TS_FLAG_STOPPED flag
  // When only using hrtimer_cancel, there is still sched_tick called from tick_nohz_handler 
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  register_kprobe(&kstep_tick_nohz_kp);
#endif
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    // Ref: tick_sched_timer_dying in
    // https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-sched.c#L1606
    struct tick_sched *ts = KSYM_tick_get_tick_sched(cpu);
    hrtimer_cancel(&ts->sched_timer);
    memset(ts, 0, sizeof(struct tick_sched));
    TRACE_INFO("Disabled timer ticks on CPU %d", cpu);
  }
}

void kstep_sleep(void) {
  if (kstep_driver->step_interval_us <= 0)
    panic("Invalid step_interval_us %llu", kstep_driver->step_interval_us);
  usleep_range(kstep_driver->step_interval_us, kstep_driver->step_interval_us);
}

static void kstep_sched_tick(void) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  KSYM_IMPORT(sched_tick);
  void *sched_tick = KSYM_sched_tick;
#else
  KSYM_IMPORT(scheduler_tick);
  void *sched_tick = KSYM_scheduler_tick;
#endif
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, sched_tick, NULL, 1);
    kstep_sleep();
  }
}

void kstep_tick(void) {
  if (kstep_driver->on_tick)
    kstep_driver->on_tick();
  if (kstep_driver->print_rq)
    kstep_print_rq();
  if (kstep_driver->print_tasks)
    kstep_print_tasks();
  if (kstep_driver->print_nr_running)
    kstep_print_nr_running();
  kstep_sched_clock_tick();
  kstep_jiffies_tick();
  kstep_sched_tick();
}

void kstep_tick_repeat(int n) {
  for (int i = 0; i < n; i++)
    kstep_tick();
}

// Tick until fn() returns non-NULL; return that.
void *kstep_tick_until(void *(*fn)(void)) {
  while (1) {
    void *result = fn();
    if (result)
      return result;
    kstep_tick();
  }
}

void *kstep_sleep_until(void *(*fn)(void)) {
  while (1) {
    void *result = fn();
    if (result)
      return result;
    kstep_sleep();
  }
}
