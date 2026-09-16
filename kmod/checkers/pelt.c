#include "checker.h"

// https://github.com/torvalds/linux/commit/17e3e88ed0b6318fde0d1c14df1a804711cab1b5
// The PELT clock runs slower than the task clock on a CPU at reduced capacity or frequency and
// catches up when the CPU idles, so idle time is always counted at full speed. A CPU whose
// utilization has reached the maximum is deemed to have no idle time to steal: the catch-up is
// booked as lost idle time and its signals keep decaying at the pace of the task clock
// (update_idle_rq_clock_pelt). The kernel skipped that step when an RT or DL task was the last to
// run, and the signals collapsed at once. The rule is over the RT and DL signals, which nothing
// detaches from (a fair task takes its share with it when it dies or migrates): from one tick to
// the next, on a CPU they alone saturated, they decay no faster than a half-life of 32 ms of
// task-clock time.
struct pelt_obs {
  unsigned long util;
  u64 clock_task;
  bool saturated;
};
static DEFINE_PER_CPU(struct pelt_obs, last_pelt);
static void check(int cpu) {
  struct rq *rq = cpu_rq(cpu);
  struct pelt_obs *last = per_cpu_ptr(&last_pelt, cpu), cur;
  u32 divider = ((LOAD_AVG_MAX - 1024) << SCHED_CAPACITY_SHIFT) - LOAD_AVG_MAX;

  cur.util = rq->avg_rt.util_avg + rq->avg_dl.util_avg;
  cur.clock_task = rq->clock_task;
  cur.saturated = rq->avg_rt.util_sum + rq->avg_dl.util_sum >= divider;
  if (last->saturated && cur.clock_task > last->clock_task) {
    u64 dt = cur.clock_task - last->clock_task, halves = dt / (32 * NSEC_PER_MSEC), rem = dt % (32 * NSEC_PER_MSEC);
    unsigned long floor = halves >= BITS_PER_LONG ? 0 : last->util >> halves;

    floor = floor * (1024 - div64_u64(rem * 1024, 32 * NSEC_PER_MSEC)) >> 10; // chord under 2^(-rem/32ms)
    if (cur.util + 16 + floor / 16 < floor)
      kstep_warn("pelt", "rt+dl util on saturated cpu %d fell %lu -> %lu over %llu ns, under the %lu decay floor",
                 cpu, last->util, cur.util, dt, floor);
  }
  *last = cur;
}

static void on_tick_end(void) {
  for_each_test_cpu(cpu)
    check(cpu);
}

void kstep_check_pelt_enable(void) { kstep_on_tick_end(on_tick_end); }
