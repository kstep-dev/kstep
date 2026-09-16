// The shared region (shm.h): allocated in the linear map so its physical address is exact and
// one range covers it all; the cli reports that address on its ready line.
#include <linux/cgroup.h>
#include <linux/cpumask.h>
#include <linux/gfp.h>
#include <linux/kstrtox.h>
#include <linux/version.h>
#include <asm/io.h>

#include "internal.h"
#include "shm.h"

static struct kstep_shm *shm;

phys_addr_t kstep_shm_init(void) {
  shm = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, get_order(sizeof(*shm)));
  if (!shm)
    panic("Failed to allocate the shared region");
  return virt_to_phys(shm);
}

u8 *kstep_shm_cov(void) { return shm->cov; }

// The cgroups: the root and every live cgroup under it, in tree order, collected into `out`.
// The tree walk needs RCU and the two control files sleep, so the walk takes the paths first and
// the files are read after it. Reading the files is how the kernel's own interface reports these
// values -- the cpu controller's weight could come from the task_group, but a cpuset's effective
// mask lives in struct cpuset, which is private to kernel/cgroup/cpuset.c. A cgroup with no cpu
// controller (the root among them, which has no cpu.weight) keeps weight 0.
static u32 shm_collect_cgroups(struct kstep_shm_cgroup *out) {
  struct cgroup_subsys_state *pos;
  struct cgroup *root;
  u32 n = 0;

  root = cgroup_get_from_path("/");
  if (IS_ERR(root))
    return 0;
  rcu_read_lock();
  css_for_each_descendant_pre(pos, &root->self) {
    if (n == KSTEP_SHM_CGROUPS)
      break;
    if (!(pos->flags & CSS_ONLINE))
      continue;
    memset(&out[n], 0, sizeof(out[n]));
    if (cgroup_path(pos->cgroup, out[n].path, sizeof(out[n].path)) < 0)
      continue; // a path that does not fit is one no kSTEP driver made
    n++;
  }
  rcu_read_unlock();
  cgroup_put(root);

  for (u32 i = 0; i < n; i++) {
    const char *name = out[i].path[1] ? out[i].path : ""; // the root's files live at CGROUP_ROOT
    char buf[64];
    cpumask_var_t mask;

    if (kstep_cgroup_read(name, "cpu.weight", buf, sizeof(buf)) > 0 &&
        kstrtouint(buf, 10, &out[i].weight))
      out[i].weight = 0; // a value that does not parse is one this kernel does not report
    if (kstep_cgroup_read(name, "cpuset.cpus.effective", buf, sizeof(buf)) > 0 &&
        zalloc_cpumask_var(&mask, GFP_KERNEL)) {
      if (!cpulist_parse(buf, mask))
        out[i].cpus = cpumask_bits(mask)[0];
      free_cpumask_var(mask);
    }
  }
  return n;
}

// The state after a command. The isolated CPUs are held while a command runs, so their queues
// can be read directly. tasks[n - 1] is the cli's task number n; exited tasks are skipped.
void kstep_shm_update(struct task_struct **tasks, int ntasks) {
  u32 ncpus = min_t(u32, num_online_cpus() - 1, KSTEP_SHM_CPUS), nt = 0; // nt: table entries written
  struct kstep_shm_cgroup cgroups[KSTEP_SHM_CGROUPS];
  u32 ncgroups = shm_collect_cgroups(cgroups); // sleeps: outside the seqlock, where a reader spins

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
        .freq = kstep_freq_get(cpu),
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
  memcpy(shm->cgroup, cgroups, ncgroups * sizeof(*cgroups));
  shm->hdr.timestamp = kstep_jiffies_get();
  shm->hdr.ncpus = ncpus;
  shm->hdr.ntasks = nt;
  shm->hdr.ncgroups = ncgroups;
  smp_wmb();
  WRITE_ONCE(shm->hdr.gen, shm->hdr.gen + 1); // even: consistent
}

