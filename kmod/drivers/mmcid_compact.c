// https://github.com/torvalds/linux/commit/77d7dc8bef48
//
// Bug: The complex CID management uses lazy-put on context switch-out: the
// cidmask bit stays set and the per-cpu CID is cached, requiring a compaction
// work item (task_tick_mm_cid -> task_work_add -> task_mm_cid_work) to remotely
// clear stale CIDs. This forces arbitrary tasks into task_work on exit to user
// space, causing latency spikes.
//
// Fix: Revert to simple bitmap allocation. On switch-out, cidmask bit is
// cleared immediately via cpumask_clear_cpu(). No compaction work needed.
//
// Observable: After pausing a task (causing switch-out), check cidmask weight:
// Buggy: cidmask weight remains 1 (CID lazy-cached, not freed from bitmap)
// Fixed: cidmask weight drops to 0 (CID freed immediately from bitmap)

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 18, 0)

#include <linux/mm_types.h>

static struct task_struct *task_a;
static struct task_struct *task_b;

static void setup(void) {
  task_a = kstep_task_create();
  task_b = kstep_task_create();
}

static void run(void) {
  kstep_task_pin(task_a, 1, 1);
  kstep_task_pin(task_b, 2, 2);

  kstep_task_wakeup(task_a);
  kstep_task_wakeup(task_b);

  // Let tasks run and get CIDs assigned
  kstep_tick_repeat(10);

  struct mm_struct *mm_a = task_a->mm;
  struct mm_struct *mm_b = task_b->mm;
  if (!mm_a || !mm_b) {
    kstep_fail("task has no mm");
    return;
  }

  int weight_a_before = cpumask_weight(mm_cidmask(mm_a));
  int weight_b_before = cpumask_weight(mm_cidmask(mm_b));
  int cid_a_before = task_a->mm_cid;
  TRACE_INFO("before pause: weight_a=%d weight_b=%d cid_a=%d",
             weight_a_before, weight_b_before, cid_a_before);

  // Pause task_a -> triggers context switch on CPU 1
  kstep_task_pause(task_a);
  kstep_tick_repeat(5);

  int weight_a_after = cpumask_weight(mm_cidmask(mm_a));
  int weight_b_after = cpumask_weight(mm_cidmask(mm_b));
  TRACE_INFO("after pause: weight_a=%d weight_b=%d cid_a=%d",
             weight_a_after, weight_b_after, task_a->mm_cid);

  // task_b should be unaffected (still running on CPU 2)
  // task_a has been paused:
  //   Buggy: cidmask weight still 1 (lazy-put, bit not cleared)
  //   Fixed: cidmask weight drops to 0 (bit cleared immediately)
  if (weight_a_after > 0) {
    kstep_fail("cidmask_weight=%d after pause (CID lazy-cached, "
               "compaction work needed to free - causes latency spikes)",
               weight_a_after);
  } else {
    kstep_pass("cidmask_weight=%d after pause (CID freed immediately, "
               "no compaction work needed)",
               weight_a_after);
  }
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "mmcid_compact",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
