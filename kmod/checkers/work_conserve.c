#include "checker.h"

// A CPU that is idle while work waits elsewhere is work the scheduler is not doing. Ported from the
// old fuzzer's work-conservation checker, which is what found local_group_imbalance.
//
// The rule holds only where the scheduler owes you the move, so it is narrow on purpose:
//   - there is at least as much runnable work as there are test CPUs, so every CPU should be busy;
//   - a CPU is idle;
//   - a task that may run there is queued behind another task;
//   - and it has been so for KSTEP_IDLE_TICKS ticks, since the balancer runs on ticks and works
//     down the domains one interval at a time: a wakeup that has not been balanced yet, or a move
//     the next interval would make, is not a violation.
// Even then it is not sound: measured over one program on even_idle_cpu it warned 175 times on the
// buggy kernel and 63 on the fixed one. Load balancing is a heuristic -- imbalance margins, cache
// affinity, capacity and misfit checks, domain intervals -- so an idle CPU beside runnable work is
// not by itself a bug. The old fuzzer used this signal with a human triaging what came out. Left
// here as a candidate generator; it is not enabled by any bug, and a finding of its own proves
// nothing until the same program is shown to be silent on the fixed kernel.
#define KSTEP_IDLE_TICKS 4 // ticks an idle CPU may sit on unclaimed work before the rule fires
static int idle_streak;

static void on_tick_end(void) {
  int ntasks;
  struct task_struct **tasks = kstep_session_tasks(&ntasks);
  int test_cpus = num_online_cpus() - 1, runnable = 0;
  struct task_struct *waiting = NULL;
  struct cpumask idle;

  cpumask_clear(&idle);
  for_each_test_cpu(cpu)
    if (cpu_rq(cpu)->nr_running == 0)
      cpumask_set_cpu(cpu, &idle);

  for (int i = 0; i < ntasks; i++) {
    struct task_struct *p = tasks[i];

    if (p->exit_state || READ_ONCE(p->__state) != TASK_RUNNING)
      continue;
    runnable++;
    if (!waiting && task_rq(p)->nr_running > 1 && cpumask_intersects(p->cpus_ptr, &idle))
      waiting = p;
  }

  if (cpumask_empty(&idle) || !waiting || runnable < test_cpus) {
    idle_streak = 0;
    return;
  }
  if (++idle_streak >= KSTEP_IDLE_TICKS)
    kstep_warn("work_conserve", "task %d queued on cpu %d behind %d others while cpu %d has been idle for %d ticks",
               waiting->pid, task_cpu(waiting), task_rq(waiting)->nr_running - 1, cpumask_first(&idle),
               idle_streak);
}

void kstep_check_work_conserve_enable(void) { kstep_on_tick_end(on_tick_end); }
