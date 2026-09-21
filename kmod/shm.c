// The shared region (shm.h): allocated in the linear map so its physical address is exact and
// one range covers it all; the cli reports that address on its ready line.
#include <linux/cgroup.h>
#include <linux/cpumask.h>
#include <linux/gfp.h>
#include <linux/kstrtox.h>
#include <linux/sched/rt.h>
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
#define TABLE(field, cap) { .max = (cap), .off = offsetof(struct kstep_shm, field), .stride = sizeof(shm->field[0]) }
  shm->hdr = (struct kstep_shm_hdr){
      .magic = KSTEP_SHM_MAGIC,
      .layout = KSTEP_SHM_LAYOUT,
      .table = {
          [KSTEP_TBL_CPU] = TABLE(cpu, KSTEP_SHM_CPUS),
          [KSTEP_TBL_TASK] = TABLE(task, KSTEP_SHM_TASKS),
          [KSTEP_TBL_CGROUP] = TABLE(cgroup, KSTEP_SHM_CGROUPS),
          [KSTEP_TBL_DOMAIN] = TABLE(domain, KSTEP_SHM_DOMAINS),
          [KSTEP_TBL_CFS] = TABLE(cfs, KSTEP_SHM_CPUS),
          [KSTEP_TBL_ENTITY] = TABLE(entity, KSTEP_SHM_ENTITIES),
          [KSTEP_TBL_RT] = TABLE(rt, KSTEP_SHM_CPUS),
          [KSTEP_TBL_RT_ENTITY] = TABLE(rt_entity, KSTEP_SHM_TASKS),
      },
      .max_groups = KSTEP_SHM_GROUPS,
      .group_stride = sizeof(struct kstep_shm_group),
  };
#undef TABLE
  return virt_to_phys(shm);
}

// The block both tables share, read straight off the entity and its queue. Eligibility, the pick
// and curr are the kernel's own answers -- entity_eligible, pick_eevdf, cfs_rq->curr -- so the
// page shows what the scheduler would do rather than a re-derivation of it. pick_eevdf is inlined
// into __pick_eevdf(cfs_rq, protect) from 6.16, where protect is RUN_TO_PARITY's guard of curr.
#ifdef CONFIG_FAIR_GROUP_SCHED
#define parent_entity(se) ((se)->parent)
#else
#define parent_entity(se) ((struct sched_entity *)NULL)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
// EEVDF's pick: __pick_eevdf(cfs_rq, protect) from 6.13, pick_eevdf(cfs_rq) before it.
static struct sched_entity *shm_pick(struct cfs_rq *cfs_rq) {
  static struct sched_entity *(*pick2)(struct cfs_rq *, bool);
  static struct sched_entity *(*pick1)(struct cfs_rq *);
  static bool looked_up;

  if (!looked_up) {
    pick2 = kstep_ksym_lookup("__pick_eevdf");
    pick1 = pick2 ? NULL : kstep_ksym_lookup("pick_eevdf");
    looked_up = true;
  }
  return pick2 ? pick2(cfs_rq, true) : pick1 ? pick1(cfs_rq) : NULL;
}
#endif

static struct kstep_shm_se shm_se(struct sched_entity *se) {
  struct cfs_rq *cfs_rq = cfs_rq_of(se);
  struct kstep_shm_se out = {
      .weight = scale_load_down(se->load.weight),
      .sum_exec_runtime = se->sum_exec_runtime,
      .vruntime = se->vruntime,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
      .deadline = se->deadline,
      .slice = se->slice,
#endif
  };

  if (se->on_rq)
    out.flags |= KSTEP_SE_ON_RQ;
  if (cfs_rq->curr == se)
    out.flags |= KSTEP_SE_CURR;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
  if (se->sched_delayed)
    out.flags |= KSTEP_SE_DELAYED;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) // EEVDF: eligibility, lag and the pick
  {
    KSYM_IMPORT(avg_vruntime); // fair.c's, the same average entity_eligible tests against
    out.lag = se->on_rq ? (s64)KSYM_avg_vruntime(cfs_rq) - (s64)se->vruntime : se->vlag;
  }
  if (se->on_rq && kstep_eligible(se))
    out.flags |= KSTEP_SE_ELIGIBLE;
  if (se->on_rq && shm_pick(cfs_rq) == se)
    out.flags |= KSTEP_SE_PICK;
#endif
  // the share of the CPU: at each level this entity's weight over everything queued there,
  // multiplied up to the root; 0 when not queued, since a queue's total counts only what is on it
  if (se->on_rq) {
    u64 share = 1024;

    for (struct sched_entity *s = se; s; s = parent_entity(s)) {
      struct cfs_rq *q = cfs_rq_of(s);
      share = q->load.weight ? div64_u64(share * s->load.weight, q->load.weight) : 0;
    }
    out.share = share;
  }
  return out;
}


// The real-time class's pick, walked the way pick_next_rt_entity does: the head of the highest
// non-empty priority list, and through a group entity (CONFIG_RT_GROUP_SCHED) into its own queue
// until a task is reached. NULL when the class has nothing runnable there -- among other reasons
// because bandwidth control took it off the CPU, which dequeues it whole (rt_queued 0).
static struct task_struct *shm_rt_pick(struct rq *rq) {
  struct rt_rq *rt_rq = &rq->rt;

  if (!rt_rq->rt_queued || !rt_rq->rt_nr_running)
    return NULL;
  for (;;) {
    struct rt_prio_array *array = &rt_rq->active;
    int idx = sched_find_first_bit(array->bitmap);
    struct sched_rt_entity *rt_se;

    if (idx >= MAX_RT_PRIO)
      return NULL;
    rt_se = list_first_entry(array->queue + idx, struct sched_rt_entity, run_list);
#ifdef CONFIG_RT_GROUP_SCHED
    if (rt_se->my_q) {
      rt_rq = rt_se->my_q;
      continue;
    }
#endif
    return container_of(rt_se, struct task_struct, rt);
  }
}

// A task's place on the real-time class: its queue is its priority's list on the rt_rq holding
// it (its task group's under CONFIG_RT_GROUP_SCHED, the CPU's otherwise), and position counts
// the entries ahead of it there.
static struct kstep_shm_rt_entity shm_rt_entity(struct task_struct *p, u32 task, struct task_struct *pick) {
  struct rq *rq = cpu_rq(task_cpu(p));
  struct kstep_shm_rt_entity out = {.task = task, .cpu = task_cpu(p), .time_slice = p->rt.time_slice};
  struct rt_rq *rt_rq = &rq->rt;
  struct list_head *pos;

#ifdef CONFIG_RT_GROUP_SCHED
  if (p->rt.rt_rq)
    rt_rq = p->rt.rt_rq;
#endif
  if (p->rt.on_rq)
    out.flags |= KSTEP_RT_ON_RQ;
  if (rq->curr == p)
    out.flags |= KSTEP_RT_CURR;
  if (pick == p)
    out.flags |= KSTEP_RT_PICK;
  if (p->rt.on_list)
    list_for_each(pos, rt_rq->active.queue + p->prio) {
      if (pos == &p->rt.run_list)
        break;
      out.position++;
    }
  return out;
}

// The cgroup's group entities, one record per test CPU, appended to `ents` -- the level the pick
// happens at, which nothing else reports: a task's own counters are kept in its group's queue, so
// four equal tasks split two cgroups' halves without anything in the task table saying why. The
// cpu controller's css is taken from the cgroup rather than the walk's, which is cgroup->self.
// Called under RCU, from a walk that runs before the seqlock is taken; the entities are stable
// because the isolated CPUs are held for the whole command.
static void shm_group_entities(u32 cgroup, struct cgroup *cgrp, struct kstep_shm_entity *ents, u32 *nents) {
#ifdef CONFIG_FAIR_GROUP_SCHED
  struct cgroup_subsys_state *css = rcu_dereference(cgrp->subsys[cpu_cgrp_id]);
  struct task_group *tg;

  if (!css)
    return;
// css_tg() is private to core.c before 6.12, when sched_ext moved it into sched.h
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
  tg = css_tg(css);
#else
  tg = container_of(css, struct task_group, css);
#endif
  if (!tg->parent) // the root task group owns no entity: its cfs_rq is the runqueue's own
    return;
  for_each_test_cpu(cpu) {
    struct sched_entity *se = tg->se[cpu];
    struct kstep_shm_entity *e = &ents[*nents];

    if (!se || *nents == KSTEP_SHM_ENTITIES)
      continue;
    *e = (struct kstep_shm_entity){
        .task = 0, // a cgroup's, not a task's
        .cgroup = cgroup,
        .cpu = cpu,
        .se = shm_se(se),
    };
    (*nents)++;
  }
#endif
}

// The cgroups: the root and every live cgroup under it, in tree order, collected into `out`.
// The tree walk needs RCU and the two control files sleep, so the walk takes the paths first and
// the files are read after it. Reading the files is how the kernel's own interface reports these
// values -- the cpu controller's weight could come from the task_group, but a cpuset's effective
// mask lives in struct cpuset, which is private to kernel/cgroup/cpuset.c. A cgroup with no cpu
// controller (the root among them, which has no cpu.weight) keeps weight 0.
static u32 shm_collect_cgroups(struct kstep_shm_cgroup *out, struct kstep_shm_entity *ents, u32 *nents) {
  struct cgroup_subsys_state *pos;
  struct cgroup *root;
  u32 n = 0;

  *nents = 0;
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
    shm_group_entities(n, pos->cgroup, ents, nents);
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
  u32 ncpus = min_t(u32, num_online_cpus() - 1, KSTEP_SHM_CPUS), nt = 0, nrt = 0; // table entries written
  struct kstep_shm_cgroup cgroups[KSTEP_SHM_CGROUPS];
  static struct kstep_shm_entity entities[KSTEP_SHM_ENTITIES]; // 40 KB: not for the stack
  struct task_struct *rt_pick[KSTEP_SHM_CPUS];
  u32 nentities;
  u32 ncgroups = shm_collect_cgroups(cgroups, entities, &nentities); // sleeps: outside the seqlock, where a reader spins

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
        // a deadline in jiffies, reported as the wait rather than the stamp: the page has no clock
        // of the kernel's, and 0 reads as "due now" whether it is due or overdue
        .next_balance_in = time_after(rq->next_balance, jiffies) ? rq->next_balance - jiffies : 0,
    };
    shm->cfs[cpu - 1] = (struct kstep_shm_cfs){
        .cpu = cpu,
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
    };
    shm->rt[cpu - 1] = (struct kstep_shm_rt){
        .cpu = cpu,
        .nr_running = rq->rt.rt_nr_running,
        .highest_prio = rq->rt.highest_prio.curr,
#ifdef CONFIG_RT_GROUP_SCHED
        .throttled = rq->rt.rt_throttled,
        .rt_time = rq->rt.rt_time,
        .rt_runtime = rq->rt.rt_runtime,
#endif
    };
    rt_pick[cpu - 1] = shm_rt_pick(rq);
  }
  for (int i = 0; i < ntasks && nt < KSTEP_SHM_TASKS; i++) {
    struct task_struct *p = tasks[i];
    struct kstep_shm_task *t = &shm->task[nt];
    char path[sizeof(cgroups[0].path)];
    unsigned int st;
    u32 cpu = task_cpu(p);

    if (p->exit_state)
      continue;
    st = READ_ONCE(p->__state);
    *t = (struct kstep_shm_task){
        .task = i + 1,
        .state = p->on_cpu ? KSTEP_TASK_RUNNING
                 : st == TASK_RUNNING ? KSTEP_TASK_RUNNABLE
                 : st & TASK_INTERRUPTIBLE ? KSTEP_TASK_SLEEPING
                 : KSTEP_TASK_BLOCKED,
        .cpu = cpu,
        .policy = p->policy,
        .nice = task_nice(p),
        .rt_priority = p->rt_priority,
        .cpus = cpumask_bits(p->cpus_ptr)[0],
        .sum_exec_runtime = p->se.sum_exec_runtime,
    };
    // the cgroup by its row in the table walked above; a task in one the walk did not fit is
    // reported as the root's, which is where the tree draws what it cannot place
    rcu_read_lock();
    if (cgroup_path(task_dfl_cgroup(p), path, sizeof(path)) >= 0)
      for (u32 g = 0; g < ncgroups; g++)
        if (!strcmp(cgroups[g].path, path)) {
          t->cgroup = g;
          break;
        }
    rcu_read_unlock();
    // the class's own record: one, in the table of the class the policy names
    if (rt_policy(p->policy))
      shm->rt_entity[nrt++] = shm_rt_entity(p, i + 1, cpu >= 1 && cpu <= ncpus ? rt_pick[cpu - 1] : NULL);
    else if ((fair_policy(p->policy) || idle_policy(p->policy)) && nentities < KSTEP_SHM_ENTITIES)
      entities[nentities++] = (struct kstep_shm_entity){
          .task = i + 1,
          .cgroup = t->cgroup,
          .cpu = cpu,
          .se = shm_se(&p->se),
      };
    nt++;
  }
  memcpy(shm->cgroup, cgroups, ncgroups * sizeof(*cgroups));
  memcpy(shm->entity, entities, nentities * sizeof(*entities));
  shm->hdr.timestamp = kstep_jiffies_get();
  shm->hdr.table[KSTEP_TBL_CPU].n = ncpus;
  shm->hdr.table[KSTEP_TBL_TASK].n = nt;
  shm->hdr.table[KSTEP_TBL_CGROUP].n = ncgroups;
  shm->hdr.table[KSTEP_TBL_DOMAIN].n = shm_collect_domains(shm->domain, ncpus);
  shm->hdr.table[KSTEP_TBL_CFS].n = ncpus;
  shm->hdr.table[KSTEP_TBL_ENTITY].n = nentities;
  shm->hdr.table[KSTEP_TBL_RT].n = ncpus;
  shm->hdr.table[KSTEP_TBL_RT_ENTITY].n = nrt;
  smp_wmb();
  WRITE_ONCE(shm->hdr.gen, shm->hdr.gen + 1); // even: consistent
}
