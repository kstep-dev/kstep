#include <linux/delay.h>
#include <linux/kprobes.h>
#include <linux/sched/clock.h>
#include <linux/sched_clock.h>
#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
#include <asm/timer.h>
#endif

#include "internal.h"

// General rq checker infrastructure
#define MAX_CPUS 16
#define MAX_CHECKERS 8
static s64 prev_checker_values[MAX_CHECKERS][MAX_CPUS];

static void rq_checkers_save(void) {
  struct kstep_checker *checkers = kstep_driver->checkers;
  if (!checkers)
    return;

  for (int i = 0; checkers[i].name && i < MAX_CHECKERS; i++) {
    switch (checkers[i].type) {
    case TEMPORAL_DELTA:
      for (int cpu = 1; cpu < num_online_cpus() && cpu < MAX_CPUS; cpu++)
        prev_checker_values[i][cpu] = checkers[i].get_value(cpu_rq(cpu));
      break;
    default:
      panic("Unsupported checker type %d", checkers[i].type);
    }
  }
}

static void check_temporal_delta(int i, struct kstep_checker *checker) {
  for (int cpu = 1; cpu < num_online_cpus() && cpu < MAX_CPUS; cpu++) {
    s64 delta = checker->get_value(cpu_rq(cpu)) - prev_checker_values[i][cpu];

    if (abs(delta) > checker->max_delta) {
      pr_warn("CHECKER %d: %s on CPU %d: delta %lld, max %lld\n", i,
              checker->name, cpu, delta, checker->max_delta);
      kstep_fail("CHECKER %d: %s on CPU %d: delta %lld, max %lld", i,
                 checker->name, cpu, delta, checker->max_delta);
    }
  }
}

static void rq_checkers_check(void) {
  struct kstep_checker *checkers = kstep_driver->checkers;
  if (!checkers)
    return;

  for (int i = 0; checkers[i].name && i < MAX_CHECKERS; i++) {
    switch (checkers[i].type) {
    case TEMPORAL_DELTA:
      check_temporal_delta(i, &checkers[i]);
      break;
    default:
      panic("Unsupported checker type %d", checkers[i].type);
    }
  }
}

static u64 kstep_sched_clock = INIT_TIME_NS;
static u64 kstep_sched_clock_read(void) { return kstep_sched_clock; }
static void kstep_sched_clock_tick(void) {
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

  kstep_jiffies_offset = nsecs_to_jiffies(kstep_sched_clock) + INITIAL_JIFFIES;
  kstep_jiffies_set(0);

  TRACE_INFO("Disabled jiffies update");
}

static void kstep_jiffies_tick(void) {
  if (jiffies != kstep_jiffies + kstep_jiffies_offset)
    panic("%lu != %llu", jiffies, kstep_jiffies + kstep_jiffies_offset);
  kstep_jiffies_set(kstep_jiffies + 1);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
static int tick_nohz_pre_handler(struct kprobe *kp, struct pt_regs *regs) {
  if (smp_processor_id() == 0)
    return 0;
  panic("tick_nohz_handler called on CPU %d", smp_processor_id());
}

static struct kprobe kstep_tick_nohz_kp = {
    .symbol_name = "tick_nohz_handler",
    .pre_handler = tick_nohz_pre_handler,
};
#endif

void kstep_sched_timer_init(void) {
  KSYM_IMPORT(tick_get_tick_sched);
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    // Ref: tick_sched_timer_dying in
    // https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-sched.c#L1606
    struct tick_sched *ts = KSYM_tick_get_tick_sched(cpu);
    hrtimer_cancel(&ts->sched_timer);
    memset(ts, 0, sizeof(struct tick_sched));
    TRACE_INFO("Disabled timer ticks on CPU %d", cpu);
  }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  register_kprobe(&kstep_tick_nohz_kp);
#endif
}

void kstep_sleep(void) {
  if (kstep_driver->step_interval_us <= 0)
    panic("Invalid step_interval_us %llu", kstep_driver->step_interval_us);
  usleep_range(kstep_driver->step_interval_us, kstep_driver->step_interval_us);
}

static void (*sched_tick_fn)(void);
static void (*sched_softirq_fn)(void);

void kstep_sched_tick_init(void) {
  sched_tick_fn =
      kstep_ksym_lookup("sched_tick") ?: kstep_ksym_lookup("scheduler_tick");
  if (!sched_tick_fn)
    panic("Failed to find sched_tick or scheduler_tick");

  sched_softirq_fn = kstep_ksym_lookup("sched_balance_softirq")
                         ?: kstep_ksym_lookup("run_rebalance_domains");
  if (!sched_softirq_fn)
    panic("Failed to find sched_balance_softirq or run_rebalance_domains");
}

static void kstep_do_sched_tick(void *data) {
  sched_tick_fn();
  // Drain SCHED_SOFTIRQ synchronously to avoid non-deterministic delivery
  if (local_softirq_pending() & (1 << SCHED_SOFTIRQ)) {
    set_softirq_pending(local_softirq_pending() & ~(1 << SCHED_SOFTIRQ));
    if (kstep_driver->on_sched_softirq_begin)
      kstep_driver->on_sched_softirq_begin();
    sched_softirq_fn();
    if (kstep_driver->on_sched_softirq_end)
      kstep_driver->on_sched_softirq_end();
  }
}

static void kstep_sched_tick(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, kstep_do_sched_tick, NULL, 1);
    kstep_sleep();
  }
}

// CFS bandwidth timer control: walk all task groups, suppress their
// real-time hrtimers, and fire the period callback at the right cadence.
#ifdef CONFIG_CFS_BANDWIDTH
static void kstep_cfs_bandwidth_tick(void) {
  typedef enum hrtimer_restart(cfs_period_timer_fn_t)(struct hrtimer *);
  KSYM_IMPORT_TYPED(cfs_period_timer_fn_t, sched_cfs_period_timer);
  KSYM_IMPORT(task_groups);

  struct task_group *tg;
  list_for_each_entry_rcu(tg, KSYM_task_groups, list) {
    struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
    if (!cfs_b->period_active)
      continue;
    if (hrtimer_active(&cfs_b->period_timer))
      hrtimer_cancel(&cfs_b->period_timer);
    u64 period_ticks = div_u64(ktime_to_ns(cfs_b->period), TICK_NSEC);
    if (period_ticks == 0 || kstep_jiffies % period_ticks == 0) {
      hrtimer_set_expires(&cfs_b->period_timer, ns_to_ktime(0));
      KSYM_sched_cfs_period_timer(&cfs_b->period_timer);
    }
  }
}
#endif

void kstep_tick(void) {
  if (kstep_driver->on_tick_begin)
    kstep_driver->on_tick_begin();
  if (kstep_driver->print_rq)
    kstep_print_rq();
  if (kstep_driver->print_tasks)
    kstep_print_tasks();
  rq_checkers_save();
  kstep_sched_clock_tick();
  kstep_jiffies_tick();
  kstep_sched_tick();
#ifdef CONFIG_CFS_BANDWIDTH
  kstep_cfs_bandwidth_tick();
#endif
  rq_checkers_check();
  if (kstep_driver->on_tick_end)
    kstep_driver->on_tick_end();
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
