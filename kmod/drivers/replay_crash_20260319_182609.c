// Replay driver for crash_20260319_182609_w0
// Reproduces: task 1 permanently stuck 'runnable' — scheduler starvation
//
// Scenario:
//   task 0  — FIFO, pinned to CPU 1, on_cpu
//   task 2  — forked from task 0, on_cpu (CPU 2)
//   task 1  — sleeping, then woken up → stuck 'runnable' indefinitely
//             even with CPUs apparently available (3-CPU QEMU instance)
//
// Starvation chain:
//   1. task 0 is forked, then pinned to CPU 1 and promoted to FIFO
//   2. task 2 inherits task 0's cgroup/cpuset, occupies CPU 2
//   3. task 1 wakes up runnable but cannot acquire any CPU slot
//   4. 50+ TICKs later task 1 still has not been scheduled
//
// ops.json: linux_name=v6.18_test, seed_id=7

#include "driver.h"
#include "op_handler.h"
#include "internal.h"

static const int ops[][4] = {
  /* initial ticks — let the scheduler settle */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */

  /* create and wake task 0 */
  {0, 0, 0, 0},  /* TASK_CREATE(0) → sleeping */
  {8, 0, 0, 0},  /* TICK */
  {6, 0, 0, 0},  /* TASK_WAKEUP(0) → on_cpu */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */

  /* create task 1 while task 0 is running */
  {0, 1, 0, 0},  /* TASK_CREATE(1) → sleeping */

  /* pause/wake task 0 to set nice before fork */
  {5, 0, 0, 0},  /* TASK_PAUSE(0) → sleeping */
  {6, 0, 0, 0},  /* TASK_WAKEUP(0) → on_cpu */
  {7, 0, -7, 0}, /* TASK_SET_PRIO(0, nice=-7) */

  /* fork task 2 from task 0; task 2 inherits cgroup/cpuset */
  {1, 0, 2, 0},  /* TASK_FORK(0 → 2) → task 2 on_cpu */
  {7, 0, 1, 0},  /* TASK_SET_PRIO(0, nice=1) */
  {8, 0, 0, 0},  /* TICK */
  {7, 0, 2, 0},  /* TASK_SET_PRIO(0, nice=2) */

  /* pause task 2, pin task 0 to CPU 1 only */
  {5, 2, 0, 0},  /* TASK_PAUSE(2) → sleeping */
  {2, 0, 1, 1},  /* TASK_PIN(0, begin=1, end=1) → task 0 locked to CPU 1 */
  {7, 0, 12, 0}, /* TASK_SET_PRIO(0, nice=12) */
  {7, 0, -1, 0}, /* TASK_SET_PRIO(0, nice=-1) */

  /* wake task 1 and task 2; task 2 back on_cpu */
  {6, 1, 0, 0},  /* TASK_WAKEUP(1) → runnable */
  {6, 2, 0, 0},  /* TASK_WAKEUP(2) → on_cpu */

  /* promote task 0 to FIFO — now task 0 holds CPU 1 indefinitely */
  {3, 0, 0, 0},  /* TASK_FIFO(0) */

  /* task 1 is 'runnable' from here; observe that it never gets scheduled:
   * task 0 (FIFO, CPU 1) and task 2 (CPU 2) hold all non-zero CPUs,
   * and task 1 cannot preempt either of them */
  {8, 0, 0, 0},  /* TICK — task 1 still runnable */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK */
  {8, 0, 0, 0},  /* TICK — task 1 still runnable after 20 ticks */
};

static void setup(void) {
  kstep_cov_init();
}

static void run(void) {
  TRACE_INFO("Running driver: starvation repro — task 1 stuck runnable");
  for (int i = 0; i < ARRAY_SIZE(ops); i++) {
    pr_info("op[%d]: %d %d %d %d\n", i,
            ops[i][0], ops[i][1], ops[i][2], ops[i][3]);
    kstep_execute_op(ops[i][0], ops[i][1], ops[i][2], ops[i][3]);
  }
}

KSTEP_DRIVER_DEFINE {
  .name = "replay_crash_20260319_182609",
  .setup = setup,
  .run = run,
  .step_interval_us = 1000,
};
