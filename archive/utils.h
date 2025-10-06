// Linux private headers
#include <kernel/sched/sched.h>

#include "logging.h"

static void print_task(struct task_struct *task) {
  TRACE_INFO(" - pid=%d, comm=%s, state=%x, vruntime=%llu", task->pid,
             task->comm, task->__state, task->se.vruntime);
}

static void print_rq(struct rq *rq) {
  TRACE_INFO("cpu=%d, nr_running=%d, nr_queued=%u, clock=%llu", cpu_of(rq),
             rq->nr_running, rq->cfs.nr_queued, rq->clock);
  struct rb_node *node = rb_first_cached(&rq->cfs.tasks_timeline);
  while (node) {
    struct sched_entity *se = rb_entry(node, struct sched_entity, run_node);
    print_task(task_of(se));
    node = rb_next(&se->run_node);
  }
}
