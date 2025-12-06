#include "kstep.h"

void print_rq_stats(void) {
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

    pr_info("print_rq_stats: CPU %d running=%d, queued=%d, switches=%3lld, "
            "avg_load=%lld, "
            "avg_util=%lu, min_vruntime=%lld, avg_vruntime=%lld\n",
            cpu, rq->nr_running - (h_nr_queued_val - h_nr_runnable_val),
            rq->nr_running, rq->nr_switches, avg_load,
            rq->avg_rt.util_avg + rq->cfs.avg.util_avg + rq->avg_dl.util_avg,
            rq->cfs.min_vruntime - INIT_TIME_NS, avg_vruntime);
  }
}

void print_tasks(void) {
  struct task_struct *p;

  pr_info("\t%3s %c%s %5s %5s %12s %12s %9s\n", "CPU", ' ', "S", "PID", "PPID",
          "vruntime", "sum-exec", "switches");
  pr_info("\t-------------------------------------------------------------\n");

  for_each_process(p) {
    if (task_cpu(p) == 0 || is_sys_kthread(p))
      continue;
    pr_info("\tprint_tasks: %3d %c%c %5d %5d %12lld %12lld %4lu+%-4lu\n",
            task_cpu(p), p->on_cpu ? '>' : ' ', task_state_to_char(p),
            task_pid_nr(p), task_ppid_nr(p), p->se.vruntime,
            p->se.sum_exec_runtime, p->nvcsw, p->nivcsw);
  }
}

void print_nr_running(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    pr_info("print_nr_running: %d %d\n", cpu, rq->nr_running);
  }
}

void print_all_tasks(void) {
  struct task_struct *p;
  pr_info("All tasks:");
  for_each_process(p) {
    pr_info("- pid=%d, cpu=%d, comm=%s", p->pid, task_cpu(p), p->comm);
  }
}
