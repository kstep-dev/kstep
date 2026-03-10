// https://github.com/torvalds/linux/commit/2cab4bd024d2
//
// Bug: print_task() in kernel/sched/debug.c prints sum_exec_runtime twice:
// once in the first SEQ_printf and again in the second, causing duplicate
// fields and misaligned columns in the runnable tasks output.

#include <linux/version.h>
#include <linux/kmsg_dump.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 11, 0)

static struct task_struct *tasks[2];

static void setup(void) {
  tasks[0] = kstep_task_create();
  tasks[1] = kstep_task_create();
}

static int count_substr(const char *hay, const char *needle) {
  int count = 0;
  size_t nlen = strlen(needle);
  while ((hay = strstr(hay, needle)) != NULL) {
    count++;
    hay += nlen;
  }
  return count;
}

static void run(void) {
  kstep_task_pin(tasks[0], 1, 1);
  kstep_task_pin(tasks[1], 1, 1);
  kstep_task_wakeup(tasks[0]);
  kstep_task_wakeup(tasks[1]);

  kstep_tick_repeat(20);

  // Dump sched_debug output to kernel log via printk
  kstep_print_sched_debug();

  // Now read back the kernel log buffer to detect the bug
  struct kmsg_dump_iter iter;
  kmsg_dump_rewind(&iter);

  char line[512];
  size_t len;
  bool has_tree_key = false;
  bool has_vruntime = false;
  bool duplicate = false;

  // SPLIT_NS uses do_div(x, 1000000): high=x/1000000, low=x%1000000
  u64 exec_ns = tasks[0]->se.sum_exec_runtime;
  char exec_str[32];
  snprintf(exec_str, sizeof(exec_str), "%9lld.%06ld",
           (long long)(exec_ns / 1000000ULL),
           (long)(exec_ns % 1000000ULL));

  char pid_str[16];
  snprintf(pid_str, sizeof(pid_str), " %d ", task_pid_nr(tasks[0]));

  while (kmsg_dump_get_line(&iter, NULL, line, sizeof(line) - 1, &len)) {
    line[len] = '\0';

    if (strstr(line, "tree-key"))
      has_tree_key = true;
    if (strstr(line, "vruntime   eligible"))
      has_vruntime = true;

    // Check for duplicate sum_exec_runtime in our task's line
    if (exec_ns > 0 && strstr(line, pid_str)) {
      int cnt = count_substr(line, exec_str);
      if (cnt >= 2) {
        duplicate = true;
        TRACE_INFO("duplicate detected: '%s' appears %d times", exec_str, cnt);
      }
    }
  }

  TRACE_INFO("tree-key=%d vruntime=%d duplicate=%d exec_str='%s'",
             has_tree_key, has_vruntime, duplicate, exec_str);

  if (has_tree_key || duplicate)
    kstep_fail("buggy output: tree-key=%d duplicate=%d", has_tree_key, duplicate);
  else
    kstep_pass("correct output: proper header, no duplicate");
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "sched_debug_output",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_nr_running,
    .step_interval_us = 10000,
};
