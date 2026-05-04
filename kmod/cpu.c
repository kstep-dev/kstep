#include <linux/cpuset.h>

#include "driver.h"
#include "internal.h"

KSYM_IMPORT_TYPED(struct sched_domain_topology_level *, sched_domain_topology);
#define for_each_tl(tl) for (tl = *KSYM_sched_domain_topology; tl->mask; tl++)

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

enum kstep_topo_level {
  KSTEP_TOPO_SMT,
  KSTEP_TOPO_CLS,
  KSTEP_TOPO_MC,
  KSTEP_TOPO_PKG,
  KSTEP_TOPO_NODE,
  KSTEP_TOPO_NR,
};

static struct cpumask kstep_masks[KSTEP_TOPO_NR][NR_CPUS];

// https://github.com/torvalds/linux/commit/661f951e371cc134ea31c84238dbdc9a898b8403
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#define DEFINE_MASKS_FN(name, level)                                            \
  static const struct cpumask *name(struct sched_domain_topology_level *tl,    \
                                    int cpu) {                                 \
    return &kstep_masks[level][cpu];                                            \
  }
#else
#define DEFINE_MASKS_FN(name, level)                                            \
  static const struct cpumask *name(int cpu) { return &kstep_masks[level][cpu]; }
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

static enum kstep_topo_level get_topo_level(const char *name) {
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
  panic("Unknown topology level %s", name);
}

static void topo_init(void) {
  int nr_cpus = num_online_cpus();
  struct sched_domain_topology_level *tl;
  for_each_tl(tl) {
    enum kstep_topo_level level = get_topo_level(tl->name);
    for (int cpu = 0; cpu < nr_cpus; cpu++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
      const struct cpumask *old_mask = tl->mask(tl, cpu);
#else
      const struct cpumask *old_mask = tl->mask(cpu);
#endif
      cpumask_copy(&kstep_masks[level][cpu], old_mask);
    }
    tl->mask = kstep_masks_fns[level];
  }

  // Isolate CPU 0: remove it from CPUs 1-N's masks at every level so
  // their load balancers never read CPU 0's non-deterministic state.
  for (int level = 0; level < KSTEP_TOPO_NR; level++) {
    cpumask_clear(&kstep_masks[level][0]);
    cpumask_set_cpu(0, &kstep_masks[level][0]);
    for (int cpu = 1; cpu < nr_cpus; cpu++)
      cpumask_clear_cpu(0, &kstep_masks[level][cpu]);
  }
}

/* Parse "cpus/cpus/..." (mutates via strsep) and apply to the given level. */
static void topo_set_level(enum kstep_topo_level level, char *spec) {
  const char *cpulists[NR_CPUS];
  int n = 0;
  char *tok;
  while ((tok = strsep(&spec, "/")) != NULL) {
    if (n >= NR_CPUS)
      panic("Too many cpulists for level %d", level);
    cpulists[n++] = tok;
  }
  if (n != num_online_cpus())
    panic("Level %d: %d cpulists, expected %d", level, n, num_online_cpus());

  for (int i = 0; i < n; i++)
    if (cpulist_parse(cpulists[i], &kstep_masks[level][i]) < 0)
      panic("Failed to parse cpulist '%s' for level %d", cpulists[i], level);

  if (level == KSTEP_TOPO_SMT) {
    // Also update cpu_sibling_map so that cpu_smt_mask() / is_core_idle()
    // reflect the new SMT topology.
    KSYM_IMPORT_TYPED(cpumask_var_t, cpu_sibling_map);
    for (int i = 0; i < n; i++)
      cpumask_copy(*per_cpu_ptr(KSYM_cpu_sibling_map, i),
                   &kstep_masks[KSTEP_TOPO_SMT][i]);
  }
}

static void topo_apply(void) {
#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
  // https://elixir.bootlin.com/linux/v6.17.8/source/drivers/base/arch_topology.c#L205-L222
  KSYM_IMPORT_TYPED(int, update_topology);
  *KSYM_update_topology = 1;
#else
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/itmt.c#L55-L56
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/smpboot.c#L129-L138
  KSYM_IMPORT(x86_topology_update);
  *KSYM_x86_topology_update = true;
#endif

  KSYM_IMPORT(rebuild_sched_domains);
  KSYM_rebuild_sched_domains();
}

void kstep_topo_set(const char *spec) {
  char buf[CPU_SPEC_LEN];
  char *cursor, *level;

  strscpy(buf, spec, sizeof(buf));
  cursor = buf;

  topo_init();
  while ((level = strsep(&cursor, "+")) != NULL) {
    char *name = strsep(&level, ":");
    if (!name || !*name || !level || !*level)
      panic("Invalid level spec in '%s'", spec);
    topo_set_level(get_topo_level(name), level);
    TRACE_INFO("applied topology %s\n", name);
  }
  topo_apply();
}

/* Parse "cpu=val[,cpu=val...]" and apply via setter. Unspecified CPUs get
 * SCHED_CAPACITY_SCALE. Each call sets the full state, not a delta. */
static void apply_per_cpu_param(const char *spec,
                                void (*set)(int cpu, int scale)) {
  char buf[CPU_SPEC_LEN];
  char *cursor, *pair;
  int nr_cpus = num_online_cpus();
  int values[NR_CPUS];

  for (int i = 0; i < nr_cpus; i++)
    values[i] = SCHED_CAPACITY_SCALE;

  strscpy(buf, spec, sizeof(buf));
  cursor = buf;
  while ((pair = strsep(&cursor, ",")) != NULL) {
    char *key = strsep(&pair, "=");
    if (!key || !*key || !pair || !*pair)
      panic("Invalid pair in spec '%s'", spec);
    int cpu, scale;
    if (kstrtoint(key, 10, &cpu) != 0 || cpu < 0 || cpu >= nr_cpus)
      panic("Invalid CPU '%s' in spec '%s'", key, spec);
    if (kstrtoint(pair, 10, &scale) != 0 || scale <= 0)
      panic("Invalid value '%s' in spec '%s'", pair, spec);
    values[cpu] = scale;
  }

  for (int cpu = 1; cpu < nr_cpus; cpu++) {
    set(cpu, values[cpu]);
    TRACE_INFO("cpu=%d scale=%d\n", cpu, values[cpu]);
  }
}

static void set_freq(int cpu, int scale) {
  if (cpu < 0 || cpu >= num_online_cpus())
    panic("cpu %d out of range", cpu);
  // x86:
  // https://elixir.bootlin.com/linux/v6.14.11/source/arch/x86/include/asm/topology.h#L287-L293
  // generic:
  // https://elixir.bootlin.com/linux/v6.14.11/source/include/linux/arch_topology.h#L33-L38
  KSYM_IMPORT(arch_freq_scale);
  *per_cpu_ptr(KSYM_arch_freq_scale, cpu) = scale;
}

static void set_cap(int cpu, int scale) {
  if (cpu < 0 || cpu >= num_online_cpus())
    panic("cpu %d out of range", cpu);
#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
  // https://elixir.bootlin.com/linux/v6.17.8/source/include/linux/topology.h#L332-L339
  per_cpu(cpu_scale, cpu) = scale;
// https://github.com/torvalds/linux/commit/5a9d10145a54f7a3fb6297c0082bf030e04db3bc
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
  static bool enabled = false;
  if (!enabled) {
    enabled = true;
    KSYM_IMPORT(arch_enable_hybrid_capacity_scale);
    KSYM_arch_enable_hybrid_capacity_scale();
  }
  KSYM_IMPORT(arch_set_cpu_capacity);
  KSYM_arch_set_cpu_capacity(cpu, scale, SCHED_CAPACITY_SCALE, scale,
                             SCHED_CAPACITY_SCALE);
#else
  panic("arch_set_cpu_capacity not supported for this kernel");
#endif
}

void kstep_cap_set(const char *spec) {
  apply_per_cpu_param(spec, set_cap);
  topo_apply();
}

void kstep_freq_set(const char *spec) {
  apply_per_cpu_param(spec, set_freq);
}

static void print_cpu_scales(void) {
  KSYM_IMPORT(arch_freq_scale);
  pr_info("CPU scales:\n");
  for (int cpu = 0; cpu < num_online_cpus(); cpu++)
    pr_info("- CPU %d: cap=%lu freq=%lu\n", cpu,
            arch_scale_cpu_capacity(cpu),
            *per_cpu_ptr(KSYM_arch_freq_scale, cpu));
}

void kstep_cpu_print(void) {
  print_topo_levels();
  print_sched_domains();
  print_cpu_scales();
}