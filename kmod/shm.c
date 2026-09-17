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
  // The shape, written once: it is a property of the build, not of any command.
  shm->hdr = (struct kstep_shm_hdr){
      .magic = KSTEP_SHM_MAGIC,
      .layout = KSTEP_SHM_LAYOUT,
      .cpu_off = offsetof(struct kstep_shm, cpu),
      .cpu_stride = sizeof(struct kstep_shm_cpu),
      .task_off = offsetof(struct kstep_shm, task),
      .task_stride = sizeof(struct kstep_shm_task),
      .cgroup_off = offsetof(struct kstep_shm, cgroup),
      .cgroup_stride = sizeof(struct kstep_shm_cgroup),
      .domain_off = offsetof(struct kstep_shm, domain),
      .domain_stride = sizeof(struct kstep_shm_domain),
      .max_cpus = KSTEP_SHM_CPUS,
      .max_tasks = KSTEP_SHM_TASKS,
      .max_cgroups = KSTEP_SHM_CGROUPS,
      .max_domains = KSTEP_SHM_DOMAINS,
      .max_groups = KSTEP_SHM_GROUPS,
      .group_stride = sizeof(struct kstep_shm_group),
  };
  return virt_to_phys(shm);
}

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

// The sched domains the kernel actually built, per CPU and innermost first. Read under RCU, like
// any other walk of rq->sd. A group's span comes from sched_group_span (the CPUs it covers), not
// its balance mask; sd->groups starts at the CPU's own group, so the order is the order that CPU's
// balancer sees.
static u32 shm_collect_domains(struct kstep_shm_domain *out, u32 ncpus) {
  u32 n = 0;

  rcu_read_lock();
  for (int cpu = 1; cpu <= ncpus && n < KSTEP_SHM_DOMAINS; cpu++) {
    struct sched_domain *sd;

    for_each_domain(cpu, sd) {
      struct kstep_shm_domain *d = &out[n];
      struct sched_group *first, *sg;

      if (n == KSTEP_SHM_DOMAINS)
        break;
      memset(d, 0, sizeof(*d));
      d->cpu = cpu;
      d->span = cpumask_bits(sched_domain_span(sd))[0];
      strscpy(d->name, sd->name, sizeof(d->name));
      kstep_sd_flags_str(sd->flags, d->flags, sizeof(d->flags));
      d->imbalance_pct = sd->imbalance_pct;
      d->balance_interval = sd->balance_interval;
      d->nr_balance_failed = sd->nr_balance_failed;
      d->busy_factor = sd->busy_factor;
      d->cache_nice_tries = sd->cache_nice_tries;
      // jiffies is the mocked clock plus a fixed offset (tick_jiffies.c), so this is logical ticks
      d->last_balance_ago = jiffies - sd->last_balance;
      first = sd->groups;
      sg = first;
      do {
        struct kstep_shm_group *g = &d->group[d->ngroups];

        g->span = cpumask_bits(sched_group_span(sg))[0];
        g->capacity = sg->sgc->capacity;
        g->min_capacity = sg->sgc->min_capacity;
        g->max_capacity = sg->sgc->max_capacity;
        g->weight = sg->group_weight;
        sg = sg->next;
      } while (++d->ngroups < KSTEP_SHM_GROUPS && sg != first);
      n++;
    }
  }
  rcu_read_unlock();
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
        // h_nr_running until 6.13 renamed it; the field the balancer reads either way
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
        .h_nr_runnable = rq->cfs.h_nr_runnable,
#else
        .h_nr_runnable = rq->cfs.h_nr_running,
#endif
        // a deadline in jiffies, reported as the wait rather than the stamp: the page has no clock
        // of the kernel's, and 0 reads as "due now" whether it is due or overdue
        .next_balance_in = time_after(rq->next_balance, jiffies) ? rq->next_balance - jiffies : 0,
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
  shm->hdr.ndomains = shm_collect_domains(shm->domain, ncpus);
  smp_wmb();
  WRITE_ONCE(shm->hdr.gen, shm->hdr.gen + 1); // even: consistent
}

