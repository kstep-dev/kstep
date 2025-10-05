// Linux private headers
#include <kernel/sched/sched.h>

#include "logging.h"

// From tools/lib/bpf/bpf_tracing.h
#if defined(CONFIG_X86_64)
#define PT_REGS_PARM1(x) ((x)->di)
#define PT_REGS_PARM2(x) ((x)->si)
#define PT_REGS_PARM3(x) ((x)->dx)
#define PT_REGS_PARM4(x) ((x)->cx)
#define PT_REGS_PARM5(x) ((x)->r8)
#define PT_REGS_RET(x) ((x)->sp)
#define PT_REGS_FP(x) ((x)->bp)
#define PT_REGS_RC(x) ((x)->ax)
#elif defined(CONFIG_ARM64)
#define PT_REGS_PARM1(x) ((x)->regs[0])
#define PT_REGS_PARM2(x) ((x)->regs[1])
#define PT_REGS_PARM3(x) ((x)->regs[2])
#define PT_REGS_PARM4(x) ((x)->regs[3])
#define PT_REGS_PARM5(x) ((x)->regs[4])
#define PT_REGS_RET(x) ((x)->regs[30])
#define PT_REGS_FP(x) ((x)->regs[29])
#define PT_REGS_RC(x) ((x)->regs[0])
#else
#error "Only support x86_64 and arm64"
#endif

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
