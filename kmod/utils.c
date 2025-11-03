#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>

#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "utils.h"

void send_sigcode(struct task_struct *p, enum sigcode code, int val) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      .si_int = val,
  };
  send_sig_info(SIGUSR1, &info, p);
  TRACE_INFO("Sent %s (si_int=%d) to pid %d", sigcode_to_str[code], val,
             p->pid);
  msleep(SIM_INTERVAL_MS);
}

struct task_struct *poll_task(const char *comm) {
  struct task_struct *p;
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, comm) == 0)
        return p;
    }
    msleep(SIM_INTERVAL_MS);
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

#if 0
struct task_struct *find_not_eligible_task(const char *comm,
                                           struct task_struct *skip_task) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, comm) != 0 || p == skip_task)
      continue;
    if (p->on_cpu == 0)
      continue;
    TRACE_DEBUG("pid=%d, eligible=%d, on_cpu=%d", p->pid,
                ksym.entity_eligible(p->se.cfs_rq, &p->se), p->on_cpu);

    if (ksym.entity_eligible(p->se.cfs_rq, &p->se) == 0) {
      return p;
    }
  }
  return NULL;
}
#endif

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
  p->se.vlag = 0;

  // reset sched avg stats
  memset(&p->se.avg, 0, sizeof(struct sched_avg));
  p->se.avg.last_update_time = INIT_TIME_NS;
  p->se.avg.load_avg = scale_load_down(p->se.load.weight);
}

void print_tasks(void) {
  struct task_struct *p;

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    TRACE_INFO("- CPU %d running=%d, switches=%3lld, clock=%lld, avg_load=%lld",
               cpu,
               rq->nr_running - (rq->cfs.h_nr_queued - rq->cfs.h_nr_runnable),
               rq->nr_switches, rq->clock, rq->cfs.avg_load);
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
