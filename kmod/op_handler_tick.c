#include "op_handler_internal.h"

u8 kstep_op_tick(int a, int b, int c) {
  (void)a;
  (void)b;
  (void)c;
  kstep_tick();
  return 1;
}

static u64 count_ineligible_cgroup_se(void) {
  u64 count = 0;

  for (int id = 0; id < MAX_CGROUPS; id++) {
    struct task_group *tg;
    if (!kstep_cgroups[id].exists)
      continue;

    tg = kstep_cgroups[id].tg;
    if (!tg)
      continue;

    for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
      struct sched_entity *se = tg->se[cpu];
      if (se && !kstep_eligible(se))
        count++;
    }
  }

  return count;
}

u8 kstep_op_tick_repeat(int a, int b, int c) {
  u8 executed_steps = 0;

  (void)b;
  (void)c;
  for (int i = 0; i < a; i++) {
    kstep_execute_op(OP_TICK, 0, 0, 0);

    /*
      Some bugs require special task/task group states to trigger.
      These conditions are hard to capture with code coverage,
      and sensitive to time (i.e. how many ticks invoked).
      Break if special state found
      These states come from studied bug set.
    */
    if (count_ineligible_cgroup_se() > (num_online_cpus() - 1))
      break;

    executed_steps++;
  }

  return executed_steps;
}
