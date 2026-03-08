#include <linux/delay.h>
#include <linux/kprobes.h>

#include "internal.h"

// General rq invariant infrastructure
#define MAX_CPUS 16
#define MAX_INVARIANTS 8
static s64 prev_invariant_values[MAX_INVARIANTS][MAX_CPUS];

static void rq_invariants_save(void) {
  struct kstep_invariant **invariants = kstep_driver->invariants;
  if (!invariants)
    return;

  for (int i = 0; invariants[i] && i < MAX_INVARIANTS; i++) {
    switch (invariants[i]->type) {
    case TEMPORAL_DELTA:
      for (int cpu = 1; cpu < num_online_cpus() && cpu < MAX_CPUS; cpu++)
        prev_invariant_values[i][cpu] = invariants[i]->get_value(cpu_rq(cpu));
      break;
    default:
      panic("Unsupported invariant type %d", invariants[i]->type);
    }
  }
}

static void check_temporal_delta(int i, struct kstep_invariant *inv) {
  for (int cpu = 1; cpu < num_online_cpus() && cpu < MAX_CPUS; cpu++) {
    s64 delta = inv->get_value(cpu_rq(cpu)) - prev_invariant_values[i][cpu];

    if (abs(delta) > inv->max_delta) {
      pr_warn("INVARIANT %d: %s on CPU %d: delta %lld, max %lld\n", i,
              inv->name, cpu, delta, inv->max_delta);
      kstep_fail("INVARIANT %d: %s on CPU %d: delta %lld, max %lld", i,
                 inv->name, cpu, delta, inv->max_delta);
    }
  }
}

static void rq_invariants_check(void) {
  struct kstep_invariant **invariants = kstep_driver->invariants;
  if (!invariants)
    return;

  for (int i = 0; invariants[i] && i < MAX_INVARIANTS; i++) {
    switch (invariants[i]->type) {
    case TEMPORAL_DELTA:
      check_temporal_delta(i, invariants[i]);
      break;
    default:
      panic("Unsupported invariant type %d", invariants[i]->type);
    }
  }
}

static void kstep_disable_sched_timer(void) {
  KSYM_IMPORT(tick_get_tick_sched);
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    // Ref: tick_sched_timer_dying in
    // https://elixir.bootlin.com/linux/v6.14/source/kernel/time/tick-sched.c#L1606
    struct tick_sched *ts = KSYM_tick_get_tick_sched(cpu);
    hrtimer_cancel(&ts->sched_timer);
    memset(ts, 0, sizeof(struct tick_sched));
    TRACE_INFO("Disabled timer ticks on CPU %d", cpu);
  }
}

static void (*sched_tick_fn)(void);
static void (*sched_softirq_fn)(void);

void kstep_tick_init(void) {
  kstep_disable_sched_timer();

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

// CFS bandwidth timer control: walk all task groups, suppress their
// real-time hrtimers, and fire the period callback at the right cadence.
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
    if (period_ticks == 0 || kstep_jiffies_get() % period_ticks == 0) {
      hrtimer_set_expires(&cfs_b->period_timer, ns_to_ktime(0));
      KSYM_sched_cfs_period_timer(&cfs_b->period_timer);
    }
  }
}

void kstep_tick(void) {
  if (kstep_driver->on_tick_begin)
    kstep_driver->on_tick_begin();
  rq_invariants_save();
  kstep_sched_clock_tick();
  kstep_jiffies_tick();
  for (int cpu = 1; cpu < num_online_cpus(); cpu++)
    smp_call_function_single(cpu, kstep_do_sched_tick, NULL, 1);
  kstep_sleep();
  kstep_cfs_bandwidth_tick();
  rq_invariants_check();
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

void kstep_sleep(void) {
  if (kstep_driver->step_interval_us <= 0)
    panic("Invalid step_interval_us %llu", kstep_driver->step_interval_us);
  usleep_range(kstep_driver->step_interval_us, kstep_driver->step_interval_us);
}

void *kstep_sleep_until(void *(*fn)(void)) {
  while (1) {
    void *result = fn();
    if (result)
      return result;
    kstep_sleep();
  }
}
