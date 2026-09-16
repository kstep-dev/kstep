#include "checker.h"

// https://github.com/torvalds/linux/commit/aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042
// A sync wakeup (WF_SYNC) says the waker is about to sleep, so wake_affine_idle() places the
// wakee on the waker's CPU when the waker is its only runnable task. With EEVDF's delayed dequeue
// a paused-but-ineligible task stays queued and counted in nr_running, so the CPU looks busy, the
// wakee lands elsewhere, and the CPU goes idle when the waker sleeps.
//
// The rule is over the placement, so it is in two halves. on_select runs where the decision is
// made and samples what it sees -- the waker's CPU and how loaded it looks -- because the
// runqueue has moved on by the time the command returns. The verdict waits for the command pass,
// where task_cpu(p) is the CPU the wakee actually ended up on: the wakee staying where it was is
// as much a misplacement as being moved elsewhere, and a rule watching migrations alone would not
// see it.
#define KSTEP_WAKEUPS 16 // pending placements in one command; a command wakes far fewer
static struct {
  struct task_struct *p;
  int waker_cpu;
} pending[KSTEP_WAKEUPS];
static atomic_t npending;

static void on_select(struct task_struct *p, int prev_cpu, int wake_flags) {
  int this_cpu = smp_processor_id();
  struct rq *rq = cpu_rq(this_cpu);
  unsigned int delayed = 0;
  int i;

  if (kstep_jiffies_get() == 0) // setup, before the mocked clock runs
    return;
  if (!(wake_flags & WF_SYNC) || this_cpu == 0 || !cpumask_test_cpu(this_cpu, p->cpus_ptr))
    return;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
  delayed = rq->cfs.h_nr_queued - rq->cfs.h_nr_runnable;
#endif
  if (rq->nr_running - delayed != 1) // the waker is not alone: wake_affine_idle is not owed the move
    return;

  i = atomic_fetch_inc(&npending); // wakeups run on the test CPUs, which step in parallel
  if (i < KSTEP_WAKEUPS) {
    get_task_struct(p); // held until the verdict: the wakee may be gone by the end of the command
    pending[i] = (typeof(pending[0])){.p = p, .waker_cpu = this_cpu};
  }
}

static void on_settle(void) {
  int n = min(atomic_xchg(&npending, 0), KSTEP_WAKEUPS);

  for (int i = 0; i < n; i++) {
    struct task_struct *p = pending[i].p;

    if (task_cpu(p) != pending[i].waker_cpu)
      kstep_warn("sync_wakeup", "sync wakeup of task %d was owed waker cpu %d, landed on %d", p->pid,
                 pending[i].waker_cpu, task_cpu(p));
    put_task_struct(p);
  }
}

void kstep_check_sync_wakeup_enable(void) {
  kstep_on_select_task_rq(on_select);
  kstep_on_settle(on_settle);
}
