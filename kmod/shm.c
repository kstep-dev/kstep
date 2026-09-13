// The shared region (shm.h): allocated in the linear map so its physical address is exact and
// one range covers it all; the cli reports that address on its ready line.
#include <linux/gfp.h>
#include <linux/version.h>
#include <asm/io.h>

#include "internal.h"
#include "shm.h"

static struct kstep_shm *shm;
static DEFINE_RAW_SPINLOCK(event_lock); // hooks on any CPU append events

phys_addr_t kstep_shm_init(void) {
  shm = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, get_order(sizeof(*shm)));
  if (!shm)
    panic("Failed to allocate the shared region");
  return virt_to_phys(shm);
}

// The state after a command. The isolated CPUs are held while a command runs, so their queues
// can be read directly. tasks[n - 1] is the cli's task number n; exited tasks are skipped.
void kstep_shm_update(struct task_struct **tasks, int ntasks) {
  u32 ncpus = min_t(u32, num_online_cpus() - 1, KSTEP_SHM_CPUS), nt = 0; // nt: table entries written

  WRITE_ONCE(shm->hdr.gen, shm->hdr.gen + 1); // odd: updating
  smp_wmb();
  for (int cpu = 1; cpu <= ncpus; cpu++) {
    struct rq *rq = cpu_rq(cpu);
    int curr = 0;

    for (int i = 0; i < ntasks; i++)
      if (tasks[i] == rq->curr)
        curr = i + 1;
    shm->cpu[cpu - 1] = (struct kstep_shm_cpu){
        .cpu = cpu,
        .curr = curr,
        .idle = rq->curr == rq->idle,
        .capacity = arch_scale_cpu_capacity(cpu),
        .nr_running = rq->nr_running,
        .nr_switches = rq->nr_switches,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)
        .min_vruntime = rq->cfs.min_vruntime,
#endif
#ifdef CONFIG_SMP
        .util_avg = rq->cfs.avg.util_avg,
        .load_avg = rq->cfs.avg.load_avg,
        .runnable_avg = rq->cfs.avg.runnable_avg,
#endif
    };
  }
  for (int i = 0; i < ntasks && nt < KSTEP_SHM_TASKS; i++) {
    struct task_struct *p = tasks[i];
    struct kstep_shm_task *t = &shm->task[nt];
    unsigned int st;
    u32 flags = 0;

    if (p->exit_state)
      continue;
    st = READ_ONCE(p->__state);
    // EEVDF: eligible = vruntime <= the queue's average; delayed = kept on the queue until eligible
    if (p->se.on_rq && kstep_eligible(&p->se))
      flags |= KSTEP_TASK_ELIGIBLE;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    if (p->se.sched_delayed)
      flags |= KSTEP_TASK_DELAYED;
#endif
    *t = (struct kstep_shm_task){
        .task = i + 1,
        .state = p->on_cpu ? KSTEP_TASK_RUNNING
                 : st == TASK_RUNNING ? KSTEP_TASK_RUNNABLE
                 : st & TASK_INTERRUPTIBLE ? KSTEP_TASK_SLEEPING
                 : KSTEP_TASK_BLOCKED,
        .cpu = task_cpu(p),
        .policy = p->policy,
        .nice = task_nice(p),
        .flags = flags,
        .cpus = cpumask_bits(p->cpus_ptr)[0],
        .weight = p->se.load.weight,
        .sum_exec_runtime = p->se.sum_exec_runtime,
        .vruntime = p->se.vruntime,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
        .deadline = p->se.deadline,
        .slice = p->se.slice,
#endif
    };
    rcu_read_lock();
    if (cgroup_path(task_dfl_cgroup(p), t->cgroup, sizeof(t->cgroup)) < 0)
      strscpy(t->cgroup, "?", sizeof(t->cgroup));
    rcu_read_unlock();
    nt++;
  }
  shm->hdr.timestamp = kstep_jiffies_get();
  shm->hdr.ncpus = ncpus;
  shm->hdr.ntasks = nt;
  smp_wmb();
  WRITE_ONCE(shm->hdr.gen, shm->hdr.gen + 1); // even: consistent
}

// A trace event, from any context: published by nevents once written.
void kstep_shm_event(u32 type, u32 task, u32 src_cpu, u32 dst_cpu, const char *name) {
  unsigned long flags;
  struct kstep_shm_event *e;

  raw_spin_lock_irqsave(&event_lock, flags);
  e = &shm->event[shm->hdr.nevents % KSTEP_SHM_EVENTS];
  *e = (struct kstep_shm_event){.timestamp = kstep_jiffies_get(), .type = type, .task = task, .src_cpu = src_cpu, .dst_cpu = dst_cpu};
  if (name)
    strscpy(e->name, name, sizeof(e->name));
  smp_wmb();
  WRITE_ONCE(shm->hdr.nevents, shm->hdr.nevents + 1);
  raw_spin_unlock_irqrestore(&event_lock, flags);
}

// on_sched_balance_selected: a CPU's balancer looked for work in a domain
void kstep_shm_balance(int cpu, struct sched_domain *sd) {
  kstep_shm_event(KSTEP_EVENT_BALANCE, 0, 0, cpu, sd->name);
}
