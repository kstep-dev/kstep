#include "internal.h"

#define for_each_tl(tl) for (tl = *ksym.sched_domain_topology; tl->mask; tl++)

static void print_sd_flags(int flags) {
#define SD_FLAG(name, meta_flag)                                               \
  if (flags & name) {                                                          \
    flags &= ~name;                                                            \
    pr_cont("%s%s", &#name[3], flags ? ", " : "");                             \
  }
#include <linux/sched/sd_flags.h>
#undef SD_FLAG
}

static void print_cpumask(const struct cpumask *mask, int width) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%*pbl", cpumask_pr_args(mask));
  pr_cont("%*s", width, buf);
}

static void print_topo_levels(void) {
  pr_info("Topology levels:\n");
  struct sched_domain_topology_level *tl;
  int nr_cpus = num_online_cpus();
  for_each_tl(tl) {
    pr_info("- %-5s| ", tl->name);
    for (int cpu = 0; cpu < nr_cpus; cpu++) {
// https://github.com/torvalds/linux/commit/661f951e371cc134ea31c84238dbdc9a898b8403
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
      const struct cpumask *mask = tl->mask(tl, cpu);
#else
      const struct cpumask *mask = tl->mask(cpu);
#endif
      print_cpumask(mask, 4);
      pr_cont(" | ");
    }
    print_sd_flags(tl->sd_flags ? (tl->sd_flags()) : 0);
    pr_cont("\n");
  }
}

static void print_sched_domain(struct sched_domain *sd) {
  pr_cont("mask=%*pbl, groups={", cpumask_pr_args(sched_domain_span(sd)));
  struct sched_group *init_sg = sd->groups;
  for (struct sched_group *sg = init_sg;; sg = sg->next) {
    print_cpumask(sched_group_span(sg), 0);
    bool last = sg->next == init_sg;
    pr_cont(": %lu%s", sg->sgc->capacity, last ? "" : ", ");
    if (last)
      break;
  }
  pr_cont("}, flags=");
  print_sd_flags(sd->flags);
  pr_cont("\n");
}

static void print_sched_domains(void) {
  pr_info("Sched domains:\n");
  struct sched_domain_topology_level *tl;
  int nr_cpus = num_online_cpus();
  for_each_tl(tl) {
    for (int cpu = 0; cpu < nr_cpus; cpu++) {
      struct sched_domain *sd;
      for_each_domain(cpu, sd) {
        if (strcmp(sd->name, tl->name) != 0)
          continue;
        pr_info("- %s[%d]: ", tl->name, cpu);
        print_sched_domain(sd);
      }
    }
  }
}

void kstep_topo_print(void) {
  print_topo_levels();
  print_sched_domains();
}

static struct cpumask kstep_masks[KSTEP_TOPO_NR][NR_CPUS];

// https://github.com/torvalds/linux/commit/661f951e371cc134ea31c84238dbdc9a898b8403
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#define DEFINE_MASKS_FN(name, type)                                            \
  static const struct cpumask *name(struct sched_domain_topology_level *tl,    \
                                    int cpu) {                                 \
    return &kstep_masks[type][cpu];                                            \
  }
#else
#define DEFINE_MASKS_FN(name, type)                                            \
  static const struct cpumask *name(int cpu) { return &kstep_masks[type][cpu]; }
#endif

DEFINE_MASKS_FN(smt_masks_fn, KSTEP_TOPO_SMT)
DEFINE_MASKS_FN(cls_masks_fn, KSTEP_TOPO_CLS)
DEFINE_MASKS_FN(mc_masks_fn, KSTEP_TOPO_MC)
DEFINE_MASKS_FN(pkg_masks_fn, KSTEP_TOPO_PKG)
DEFINE_MASKS_FN(node_masks_fn, KSTEP_TOPO_NODE)

static sched_domain_mask_f kstep_masks_fns[KSTEP_TOPO_NR] = {
    [KSTEP_TOPO_SMT] = smt_masks_fn,   [KSTEP_TOPO_CLS] = cls_masks_fn,
    [KSTEP_TOPO_MC] = mc_masks_fn,     [KSTEP_TOPO_PKG] = pkg_masks_fn,
    [KSTEP_TOPO_NODE] = node_masks_fn,
};

static enum kstep_topo_type get_topo_type(const char *name) {
  if (strcmp(name, "SMT") == 0)
    return KSTEP_TOPO_SMT;
  if (strcmp(name, "CLS") == 0)
    return KSTEP_TOPO_CLS;
  if (strcmp(name, "MC") == 0)
    return KSTEP_TOPO_MC;
  // https://github.com/torvalds/linux/commit/f577cd57bfaa889cf0718e30e92c08c7f78c9d85
  if (strcmp(name, "PKG") == 0 || strcmp(name, "DIE") == 0)
    return KSTEP_TOPO_PKG;
  if (strcmp(name, "NODE") == 0)
    return KSTEP_TOPO_NODE;
  panic("Unknown topology type %s", name);
}

void kstep_topo_init(void) {
  int nr_cpus = num_online_cpus();
  struct sched_domain_topology_level *tl;
  for_each_tl(tl) {
    for (int cpu = 0; cpu < nr_cpus; cpu++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
      const struct cpumask *old_mask = tl->mask(tl, cpu);
#else
      const struct cpumask *old_mask = tl->mask(cpu);
#endif
      cpumask_copy(&kstep_masks[get_topo_type(tl->name)][cpu], old_mask);
    }
    tl->mask = kstep_masks_fns[get_topo_type(tl->name)];
  }
}

void kstep_topo_set_level(enum kstep_topo_type type, const char *cpulists[],
                          int size) {
  if (size != num_online_cpus())
    panic("size %d != num_online_cpus %d", size, num_online_cpus());
  for (int i = 0; i < size; i++)
    if (cpulist_parse(cpulists[i], &kstep_masks[type][i]) < 0)
      panic("Failed to parse cpulist %s for level %d", cpulists[i], type);
}

void kstep_topo_apply(void) {
#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
  // https://elixir.bootlin.com/linux/v6.17.8/source/drivers/base/arch_topology.c#L205-L222
  *ksym.update_topology = 1;
#else
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/itmt.c#L55-L56
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/smpboot.c#L129-L138
  *ksym.x86_topology_update = true;
#endif

  ksym.rebuild_sched_domains();
}

void kstep_cpu_set_freq(int cpu, int scale) {
  if (cpu < 0 || cpu >= num_online_cpus())
    panic("cpu %d out of range", cpu);
  // x86:
  // https://elixir.bootlin.com/linux/v6.14.11/source/arch/x86/include/asm/topology.h#L287-L293
  // generic:
  // https://elixir.bootlin.com/linux/v6.14.11/source/include/linux/arch_topology.h#L33-L38
  *per_cpu_ptr(ksym.arch_freq_scale, cpu) = scale;
}

void kstep_cpu_set_capacity(int cpu, int scale) {
  if (cpu < 0 || cpu >= num_online_cpus())
    panic("cpu %d out of range", cpu);
#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
  // https://elixir.bootlin.com/linux/v6.17.8/source/include/linux/topology.h#L332-L339
  per_cpu(cpu_scale, cpu) = scale;
#else
  static bool enabled = false;
  if (!enabled) {
    enabled = true;
    // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/cpu/aperfmperf.c#L362-L393
    ksym.arch_enable_hybrid_capacity_scale();
  }
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/cpu/aperfmperf.c#L395-L422
  ksym.arch_set_cpu_capacity(cpu, scale, SCHED_CAPACITY_SCALE, scale,
                             SCHED_CAPACITY_SCALE);
#endif
}
