#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/version.h>

#include "internal.h"
#include "ksym.h"
#include "logging.h"

void send_sigcode3(struct task_struct *p, enum sigcode code, int val1, int val2,
                   int val3) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      ._sifields = {._rt = {._sigval = {val1}, ._pid = val2, ._uid = val3}}};
  send_sig_info(SIGUSR1, &info, p);
  TRACE_INFO("Sent %s (val1=%d, val2=%d, val3=%d) to pid %d",
             sigcode_to_str[code], val1, val2, val3, p->pid);
  udelay(SIM_INTERVAL_US);
  yield(); // yield to let the task (e.g. busy during its init, cgroup
           // controller uthread) run
}

struct task_struct *poll_task(const char *comm) {
  struct task_struct *p;
  // for_each_process(p) {
  //   TRACE_DEBUG("pid=%d, comm=%s, state=%x, on_cpu=%d", p->pid, p->comm,
  //               p->__state, p->on_cpu);
  // }
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, comm) == 0)
        return p;
    }
    udelay(SIM_INTERVAL_US);
    yield(); // busy might be blocked by the busy controller, yield to let it
             // run
    TRACE_INFO("Waiting for process %s to be created", comm);
  }
}

const struct cpumask *cpu_controlled_mask;
static struct cpumask cpu_controlled_mask_data;

void cpu_controlled_mask_init(void) {
  cpumask_copy(&cpu_controlled_mask_data, cpu_active_mask);
  cpumask_clear_cpu(0, &cpu_controlled_mask_data);
  cpu_controlled_mask = &cpu_controlled_mask_data;
}

void reset_task_stats(struct task_struct *p) {
  // reset generic task stats
  p->nivcsw = 0;
  p->nvcsw = 0;

  // reset sched entity stats
  p->se.exec_start = 0;
  p->se.sum_exec_runtime = 0;
  p->se.prev_sum_exec_runtime = 0;
  p->se.nr_migrations = 0;
  p->se.vruntime = INIT_TIME_NS;
  // p->se.deadline = INIT_TIME_NS;

// https://github.com/torvalds/linux/commit/86bfbb7ce4f67a88df2639198169b685668e7349
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  p->se.vlag = 0;
#endif

  // reset sched avg stats
  memset(&p->se.avg, 0, sizeof(struct sched_avg));
  p->se.avg.last_update_time = INIT_TIME_NS;
  p->se.avg.load_avg = scale_load_down(p->se.load.weight);
}

void print_tasks(void) {
  struct task_struct *p;

  TRACE_INFO("sched_clock=%lld, jiffies=%lu", sched_clock(),
             jiffies - INITIAL_JIFFIES);

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);

    int h_nr_runnable_val = 0, h_nr_queued_val = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
    h_nr_runnable_val = rq->cfs.h_nr_runnable;
    h_nr_queued_val = rq->cfs.h_nr_queued;
#endif

// https://github.com/torvalds/linux/commit/af4cf40470c22efa3987200fd19478199e08e103
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    u64 avg_load = rq->cfs.avg_load;
    u64 avg_vruntime = ksym.avg_vruntime(&rq->cfs) - INIT_TIME_NS;
#else
    u64 avg_load = 0;
    u64 avg_vruntime = 0;
#endif

    TRACE_INFO("- CPU %d running=%d, queued=%d, switches=%3lld, avg_load=%lld, "
               "avg_util=%lu, min_vruntime=%lld, avg_vruntime=%lld",
               cpu, rq->nr_running - (h_nr_queued_val - h_nr_runnable_val),
               rq->nr_running,
               rq->nr_switches, avg_load,
               rq->avg_rt.util_avg + rq->cfs.avg.util_avg + rq->avg_dl.util_avg,
               rq->cfs.min_vruntime - INIT_TIME_NS, avg_vruntime);
  }

  int min_pid = INT_MAX;
  for_each_process(p) {
    if (task_cpu(p) == 0 || is_sys_kthread(p))
      continue;
    if (task_pid_nr(p) < min_pid)
      min_pid = task_pid_nr(p);
  }

  TRACE_DEBUG("\t%3s %c%s %5s %5s %12s %12s %9s", "CPU", ' ', "S", "PID",
              "PPID", "vruntime", "sum-exec", "switches");
  TRACE_DEBUG(
      "\t-------------------------------------------------------------");

  for_each_process(p) {
    if (task_cpu(p) == 0 || is_sys_kthread(p))
      continue;
    TRACE_DEBUG("\t%3d %c%c %5d %5d %12lld %12lld %4lu+%-4lu", task_cpu(p),
                p->on_cpu ? '>' : ' ', task_state_to_char(p),
                task_pid_nr(p) - min_pid, task_ppid_nr(p), p->se.vruntime,
                p->se.sum_exec_runtime, p->nvcsw, p->nivcsw);
  }
}

static char *sys_kthread_comms[] = {
    "cpuhp/",
    "migration/",
    "ksoftirqd/",
};

int is_sys_kthread(struct task_struct *p) {
  for (int i = 0; i < ARRAY_SIZE(sys_kthread_comms); i++) {
    if (strstarts(p->comm, sys_kthread_comms[i]))
      return 1;
  }
  return 0;
}
