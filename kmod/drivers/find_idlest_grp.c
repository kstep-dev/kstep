// Reproduce: sgs->group_weight uninitialized in update_sg_wakeup_stats()
// (commit 289de3598481)
//
// Bug: update_sg_wakeup_stats() memsets sgs to 0 but never sets
// sgs->group_weight before calling group_classify(). With group_weight=0,
// group_has_capacity() fails its first check (sum_nr_running < 0 is always
// false), falling through to the utilization check. A group with fewer
// running tasks than CPUs but high utilization is misclassified as
// group_fully_busy (or even group_overloaded) instead of group_has_spare.
//
// Setup: MC group A = CPUs 1-6 (5 tasks on CPUs 1-5, CPU 6 idle),
//        MC group B = CPUs 7-8 (parent on 7, extra on 8).
// Fork from parent -> child should go to group A (has spare capacity)
// but with the bug, group A is misclassified, so child stays on group B.

#include "driver.h"
#include "internal.h"
#include <linux/sched/signal.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 6, 0)

static struct task_struct *remote_tasks[5];
static struct task_struct *parent_task;
static struct task_struct *local_busy;

static void setup(void) {
  kstep_topo_init();
  // MC groups: {0}, {1-6}, {1-6}x5, {7-8}, {7-8}
  const char *mc[] = {"0",   "1-6", "1-6", "1-6", "1-6",
                       "1-6", "1-6", "7-8", "7-8"};
  kstep_topo_set_mc(mc, ARRAY_SIZE(mc));
  kstep_topo_apply();
  kstep_topo_print();

  for (int i = 0; i < 5; i++)
    remote_tasks[i] = kstep_task_create();
  parent_task = kstep_task_create();
  local_busy = kstep_task_create();
}

static void *find_child(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (p->real_parent == parent_task && p != parent_task)
      return p;
  }
  return NULL;
}

static void run(void) {
  // Pin remote tasks to CPUs 1-5 (CPU 6 stays idle in remote group)
  for (int i = 0; i < 5; i++)
    kstep_task_pin(remote_tasks[i], i + 1, i + 1);

  // Pause local tasks to keep their util at zero during remote warmup
  kstep_task_pause(parent_task);
  kstep_task_pause(local_busy);

  // Build max utilization on remote CPUs 1-5
  kstep_tick_repeat(300);

  TRACE_INFO("After remote warmup: rq1_util=%lu rq5_util=%lu rq6_util=%lu",
             cpu_rq(1)->cfs.avg.util_avg, cpu_rq(5)->cfs.avg.util_avg,
             cpu_rq(6)->cfs.avg.util_avg);

  // Wake local tasks by pinning (PIN signal interrupts PAUSE handler's pause())
  kstep_task_pin(parent_task, 7, 7);
  kstep_task_pin(local_busy, 8, 8);

  // Build moderate util on local CPUs 7-8
  kstep_tick_repeat(10);

  TRACE_INFO("After local warmup: rq7_util=%lu rq8_util=%lu",
             cpu_rq(7)->cfs.avg.util_avg, cpu_rq(8)->cfs.avg.util_avg);

  // Widen parent's affinity so the forked child can be placed on any CPU
  kstep_task_pin(parent_task, 1, 8);

  // Fork one child from parent (still on CPU 7)
  kstep_task_fork(parent_task, 1);

  // Wait for child to appear
  struct task_struct *child = kstep_sleep_until(find_child);

  int child_cpu = task_cpu(child);
  int parent_cpu = task_cpu(parent_task);

  TRACE_INFO("Parent on CPU %d, Child on CPU %d", parent_cpu, child_cpu);

  // With bug: remote group (CPUs 1-6) is misclassified because
  //   sgs->group_weight = 0 (memset, never set)
  //   group_is_overloaded: 5 <= 0 is false -> util check passes -> overloaded!
  //   So remote = group_overloaded, local = group_has_spare
  //   local < remote -> return NULL -> child stays local (CPUs 7-8)
  //
  // With fix: sgs->group_weight = 6 (correctly set)
  //   group_is_overloaded: 5 <= 6 true -> false
  //   group_has_capacity: 5 < 6 true -> group_has_spare
  //   Both groups are group_has_spare -> compare idle_cpus
  //   remote idle_cpus=1 > local idle_cpus=0 -> child goes to remote
  if (child_cpu >= 1 && child_cpu <= 6) {
    kstep_pass("Child on CPU %d (remote group) - group_weight correctly set",
               child_cpu);
  } else {
    kstep_fail("Child on CPU %d (local group) - group_weight=0 bug: "
               "remote group misclassified",
               child_cpu);
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "find_idlest_grp",
    .setup = setup,
    .run = run,
    .step_interval_us = 100,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "find_idlest_grp",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
