void print_rq_json(struct rq *rq) {
    int h_nr_runnable_val = 0, h_nr_queued_val = 0, h_nr_idle_val = 0,
        nr_queued_val = 0;
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
    h_nr_runnable_val = rq->cfs.h_nr_runnable;
    h_nr_queued_val = rq->cfs.h_nr_queued;
    h_nr_idle_val = rq->cfs.h_nr_idle;
    nr_queued_val = rq->cfs.nr_queued;
  #endif
  
  // https://github.com/torvalds/linux/commit/11137d384996bb05cf33c8163db271e1bac3f4bf
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
    unsigned int util_est = rq->cfs.avg.util_est;
  #else
    unsigned int util_est = rq->cfs.avg.util_est.enqueued;
  #endif
  
  // https://github.com/torvalds/linux/commit/af4cf40470c22efa3987200fd19478199e08e103
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    u64 avg_vruntime = rq->cfs.avg_vruntime;
  #else
    u64 avg_vruntime = 0;
  #endif
  
    pr_info("{"
            "\"rq[%d]\": "
            "{"
            "\"nr_running\": %d, "
            "\"nr_switches\": %llu, "
            "\"nr_uninterruptible\": %d, "
            "\"next_balance\": %lu, "
            "\"curr->pid\": %d, "
            "\"clock\": %llu, "
            "\"clock_task\": %llu, "
            "\"avg_idle\": %llu, "
            "\"max_idle_balance_cost\": %llu"
            "}"
            "}",
            rq->cpu, rq->nr_running, rq->nr_switches, rq->nr_uninterruptible,
            rq->next_balance, rq->curr->pid, rq->clock, rq->clock_task,
            rq->avg_idle, rq->max_idle_balance_cost);
    pr_info("{"
            "\"cfs_rq[%d]\": "
            "{"
            "\"min_vruntime\": %llu, "
            "\"avg_vruntime\": %llu, "
            "\"nr_queued\": %u, "
            "\"h_nr_runnable\": %u, "
            "\"h_nr_queued\": %u, "
            "\"h_nr_idle\": %u, "
            "\"load\": %lu, "
            "\"load_avg\": %lu, "
            "\"runnable_avg\": %lu, "
            "\"util_avg\": %lu, "
            "\"util_est\": %d, "
            "\"removed.load_avg\": %lu, "
            "\"removed.util_avg\": %lu, "
            "\"removed.runnable_avg\": %lu, "
            "\"tg_load_avg_contrib\": %lu, "
            "\"tg_load_avg\": %ld"
            "}"
            "}",
            rq->cpu, rq->cfs.min_vruntime, avg_vruntime, nr_queued_val,
            h_nr_runnable_val, h_nr_queued_val, h_nr_idle_val,
            rq->cfs.load.weight, rq->cfs.avg.load_avg, rq->cfs.avg.runnable_avg,
            rq->cfs.avg.util_avg, util_est, rq->cfs.removed.load_avg,
            rq->cfs.removed.util_avg, rq->cfs.removed.runnable_avg,
            rq->cfs.tg_load_avg_contrib, atomic_long_read(&rq->cfs.tg->load_avg));
    pr_info("{\"rt_rq[%d]\": {\"rt_nr_running\": %u}}", rq->cpu,
            rq->rt.rt_nr_running);
    pr_info("{\"dl_rq[%d]\": {\"dl_nr_running\": %u, \"bw\": %llu, "
            "\"total_bw\": %llu}}",
            rq->cpu, rq->dl.dl_nr_running, rq->rd->dl_bw.bw,
            rq->rd->dl_bw.total_bw);
  }
  
  void print_task_json(struct task_struct *p) {
  
  // https://github.com/torvalds/linux/commit/147f3efaa24182a21706bca15eab2f3f4630b5fe
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    u64 deadline = p->se.deadline;
    u64 slice = p->se.slice;
  #else
    u64 deadline = 0;
    u64 slice = 0;
  #endif
  
    pr_info("{"
            "\"tasks[%d]\": "
            "{"
            "\"comm\": \"%s\", "
            "\"pid\": %d, "
            "\"ppid\": %d, "
            "\"cpu\": %d, "
            "\"on_cpu\": %d, "
            "\"task_state\": \"%c\", "
            "\"vruntime\": %llu, "
            "\"deadline\": %llu, "
            "\"slice\": %llu, "
            "\"sum-exec\": %llu, "
            "\"switches\": %lu, "
            "\"prio\": %d, "
            "\"node\": %d"
            "}"
            "}",
            task_pid_nr(p), p->comm, task_pid_nr(p), task_ppid_nr(p), task_cpu(p),
            p->on_cpu, task_state_to_char(p), p->se.vruntime, deadline, slice,
            p->se.sum_exec_runtime, p->nvcsw + p->nivcsw, p->prio, task_node(p));
  }
  
  void print_sd_json(struct sched_domain *sd) {
    pr_info("{"
            "\"sd[\"%s\"]\": "
            "{"
            "\"last_balance\": %lu, "
            "\"balance_interval\": %u, "
            "\"nr_balance_failed\": %u, "
            "\"min_interval\": %lu, "
            "\"max_interval\": %lu, "
            "\"max_newidle_lb_cost\": %llu, "
            "\"busy_factor\": %u, "
            "\"imbalance_pct\": %u, "
            "\"cache_nice_tries\": %u, "
            "\"imb_numa_nr\": %u, "
            "\"nohz_idle\": %d, "
            "\"flags\": %u, "
            "\"level\": %u, "
            "\"span_weight\": %u "
            "}"
            "}",
            sd->name, sd->last_balance, sd->balance_interval,
            sd->nr_balance_failed, sd->min_interval, sd->max_interval,
            sd->max_newidle_lb_cost, sd->busy_factor, sd->imbalance_pct,
            sd->cache_nice_tries, sd->imb_numa_nr, sd->nohz_idle, sd->flags,
            sd->level, sd->span_weight);
  }
  
  void print_sched_state_json(void) {
    for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
      print_rq_json(cpu_rq(cpu));
    }
    for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
      struct sched_domain *sd;
      for_each_domain(cpu, sd) { print_sd_json(sd); }
    }
    struct task_struct *g;
    struct task_struct *p;
    for_each_process_thread(g, p) {
      if (task_cpu(p) == 0)
        continue;
      print_task_json(p);
    }
  }
