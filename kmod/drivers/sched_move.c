// https://github.com/torvalds/linux/commit/76f970ce51c8
//
// Bug: An optimization in sched_move_task() added an early bailout when
// p->sched_task_group == sched_get_task_group(p), skipping the dequeue/
// sched_change_group/enqueue sequence. This prevented detach_task_cfs_rq()
// from being called during sched_autogroup_exit_task() for exiting tasks,
// causing stale group utilization in the root cfs_rq. Rapid fork/exit
// sequences accumulate this stale utilization, causing the load balancer
// to see inflated group_util values.
//
// Fix: Revert the optimization so sched_move_task() always performs the
// full dequeue/sched_change_group/enqueue, properly updating utilization.
//
// Observable: After many fork/exit cycles, root cfs_rq util_avg is
// inflated on buggy kernel (stale util not detached) vs properly cleaned
// on fixed kernel.

#include <linux/version.h>

#include "driver.h"
#include "internal.h"
#include "../user.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)

#include <linux/sched/signal.h>

KSYM_IMPORT_TYPED(unsigned int, sysctl_sched_autogroup_enabled);

static struct task_struct *parent;

static void task_exit(struct task_struct *p) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = SIGCODE_EXIT,
  };
  send_sig_info(SIGUSR1, &info, p);
}

static void setup(void) {
  // Disable autogroup via sysctl so that sched_get_task_group() returns
  // the cgroup task_group (same as p->sched_task_group). This triggers
  // the buggy early bail in sched_move_task() during
  // sched_autogroup_exit_task().
  *KSYM_sysctl_sched_autogroup_enabled = 0;

  // 4 CPUs: CPU 0 reserved, CPUs 1-3 in a single MC domain
  kstep_topo_init();
  const char *mc[] = {"0", "1-3", "1-3", "1-3"};
  kstep_topo_set_mc(mc, 4);
  kstep_topo_apply();

  parent = kstep_task_create();
}

static int kill_children(struct task_struct *p) {
  int count = 0;
  struct task_struct *child;
  list_for_each_entry(child, &p->children, sibling) {
    task_exit(child);
    count++;
  }
  return count;
}

static void run(void) {
  // Pin parent to CPU 1
  kstep_task_pin(parent, 1, 1);
  kstep_tick_repeat(5);

  // Record baseline util before fork/exit cycles
  unsigned long baseline_util = cpu_rq(1)->cfs.avg.util_avg;
  TRACE_INFO("baseline util_avg=%lu", baseline_util);

  // Rapid fork/exit cycles to accumulate stale utilization.
  // With sched_autogroup_enabled=0, sched_get_task_group() returns the
  // cgroup tg which matches p->sched_task_group. On the buggy kernel,
  // sched_move_task() bails early, skipping detach_task_cfs_rq().
  // This leaves stale util in the group cfs_rq that propagates to the
  // root cfs_rq.
  for (int round = 0; round < 20; round++) {
    kstep_task_fork(parent, 3);
    kstep_tick_repeat(15);

    int killed = kill_children(parent);
    kstep_tick_repeat(5);
    kstep_sleep();
    kstep_sleep();

    unsigned long util = cpu_rq(1)->cfs.avg.util_avg;
    TRACE_INFO("round %d: killed=%d util_avg=%lu", round, killed, util);
  }

  // Pause parent to remove its active contribution
  kstep_task_pause(parent);
  kstep_tick_repeat(30);

  unsigned long final_util = cpu_rq(1)->cfs.avg.util_avg;
  TRACE_INFO("Final (parent paused): CPU1 util_avg=%lu", final_util);

  // On buggy kernel: stale util lingers because detach_task_cfs_rq was
  // skipped during each sched_autogroup_exit_task call
  // On fixed kernel: sched_move_task always runs the full dequeue/
  // sched_change_group/enqueue, properly cleaning up util
  if (final_util > 200) {
    kstep_fail("stale util_avg=%lu after fork/exit cycles "
               "(detach_task_cfs_rq skipped in sched_move_task)",
               final_util);
  } else {
    kstep_pass("util_avg=%lu properly cleaned after fork/exit cycles",
               final_util);
  }

  kstep_tick_repeat(10);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

static void on_tick_begin(void) {
  kstep_json_print_2kv("type", "util_avg", "val", "%llu",
                        cpu_rq(1)->cfs.avg.util_avg);
}

KSTEP_DRIVER_DEFINE{
    .name = "sched_move",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 1000,
};
