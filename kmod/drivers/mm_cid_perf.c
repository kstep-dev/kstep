// https://github.com/torvalds/linux/commit/223baf9d17f25e2608dbdff7232c095c1e612268
//
// Bug: The initial mm_cid implementation frees and reallocates concurrency IDs
// on every context switch via mm->cid_lock, causing severe spinlock contention.
// The fix introduces per-mm/per-cpu cid caching with lazy reclamation.
//
// Observable: On the buggy kernel, cidmask bits are cleared immediately when a
// task switches out. On the fixed kernel, cidmask bits persist (per-cpu cached).
// After many ticks with alternating tasks on the same CPU, the total cidmask
// weight across all task mm's is higher on the fixed kernel.

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 3, 0)

#include <linux/mm_types.h>

static struct task_struct *tasks[4];
static int tick_count;

static void setup(void) {
  for (int i = 0; i < 4; i++)
    tasks[i] = kstep_task_create();
}

static int get_total_cidmask_weight(void) {
  int total = 0;
  for (int i = 0; i < 4; i++) {
    struct mm_struct *mm = tasks[i]->mm;
    if (mm) {
      cpumask_t *cidmask = mm_cidmask(mm);
      total += cpumask_weight(cidmask);
    }
  }
  return total;
}

static void run(void) {
  // Pin 2 tasks from different mm's on CPU 1, 2 on CPU 2
  kstep_task_pin(tasks[0], 1, 1);
  kstep_task_pin(tasks[1], 1, 1);
  kstep_task_pin(tasks[2], 2, 2);
  kstep_task_pin(tasks[3], 2, 2);

  for (int i = 0; i < 4; i++)
    kstep_task_wakeup(tasks[i]);

  // Warm up: many ticks to establish stable scheduling pattern
  tick_count = 0;
  kstep_tick_repeat(60);

  // Check cidmask weight for each task's mm
  int total = get_total_cidmask_weight();
  for (int i = 0; i < 4; i++) {
    struct mm_struct *mm = tasks[i]->mm;
    if (mm) {
      TRACE_INFO("task[%d] pid=%d cidmask_weight=%d mm_cid=%d on_cpu=%d",
                 i, tasks[i]->pid, cpumask_weight(mm_cidmask(mm)),
                 tasks[i]->mm_cid, tasks[i]->on_cpu);
    }
  }

  TRACE_INFO("total cidmask weight = %d", total);

  // Buggy: only the 2 currently-running tasks retain cidmask bits (total <= 2)
  // Fixed: all 4 tasks retain cached cidmask bits (total == 4)
  if (total <= 2)
    kstep_fail("mm_cid perf regression: total cidmask_weight=%d "
               "(cids freed on every ctx switch, causes spinlock contention)",
               total);
  else
    kstep_pass("mm_cid optimized: total cidmask_weight=%d "
               "(per-cpu cid caching eliminates spinlock contention)",
               total);

  kstep_tick_repeat(10);
}

static void on_tick_begin(void) {
  tick_count++;
  int total = get_total_cidmask_weight();
  kstep_json_print_2kv("type", "mm_cid_weight", "val", "%d", total);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
static void on_tick_begin(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "mm_cid_perf",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 1000,
};
