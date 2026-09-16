#include "checker.h"

// https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3
// should_we_balance() elects one CPU per group to pull load: an idle one when there is one. The
// rule: a busy CPU does not balance while a CPU of its own group is idle.
//
// The balance_selected event is raised where should_we_balance() has already made its choice.
static void on_balance(int cpu, struct sched_domain *sd) {
  struct sched_group *sg = sd->groups;
  int i;

  if (cpu_rq(cpu)->nr_running == 0 || !cpumask_test_cpu(cpu, sched_group_span(sg)))
    return;
  for_each_cpu(i, sched_group_span(sg))
    if (cpu_rq(i)->nr_running == 0)
      return kstep_warn("balance", "cpu %d (%d running) balances while cpu %d in its group is idle", cpu,
                        cpu_rq(cpu)->nr_running, i);
}

void kstep_check_balance_enable(void) { kstep_on_balance_selected(on_balance); }
