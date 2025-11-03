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
  p->nivcsw = 0;
  p->nvcsw = 0;
  p->se.sum_exec_runtime = 0;
  p->se.vruntime = INIT_TIME_NS;
  p->se.deadline = INIT_TIME_NS;
}
