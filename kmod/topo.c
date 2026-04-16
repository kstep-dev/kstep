#include <linux/cpuset.h>

#include "driver.h"
#include "internal.h"

#define EXECUTOR_TOPOLOGY_LEN 512
static char executor_topology[EXECUTOR_TOPOLOGY_LEN] = "";
module_param_string(topology, executor_topology, EXECUTOR_TOPOLOGY_LEN, 0644);

#define EXECUTOR_FREQUENCY_LEN 256
static char executor_frequency[EXECUTOR_FREQUENCY_LEN] = "";
module_param_string(frequency, executor_frequency, EXECUTOR_FREQUENCY_LEN, 0644);

#define EXECUTOR_CAPACITY_LEN 256
static char executor_capacity[EXECUTOR_CAPACITY_LEN] = "";
module_param_string(capacity, executor_capacity, EXECUTOR_CAPACITY_LEN, 0644);

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

void kstep_topo_print(void) {
  print_topo_levels();
  print_sched_domains();
}

enum kstep_topo_type {
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
    enum kstep_topo_type type = get_topo_type(tl->name);
    for (int cpu = 0; cpu < nr_cpus; cpu++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
      const struct cpumask *old_mask = tl->mask(tl, cpu);
#else
      const struct cpumask *old_mask = tl->mask(cpu);
#endif
      cpumask_copy(&kstep_masks[type][cpu], old_mask);
    }
    tl->mask = kstep_masks_fns[type];
  }

  // Isolate CPU 0: remove it from CPUs 1-N's masks at every level so
  // their load balancers never read CPU 0's non-deterministic state.
  for (int type = 0; type < KSTEP_TOPO_NR; type++) {
    cpumask_clear(&kstep_masks[type][0]);
    cpumask_set_cpu(0, &kstep_masks[type][0]);
    for (int cpu = 1; cpu < nr_cpus; cpu++)
      cpumask_clear_cpu(0, &kstep_masks[type][cpu]);
  }
}

static void kstep_topo_set_level(enum kstep_topo_type type,
                                 const char *cpulists[], int size) {
  if (size != num_online_cpus())
    panic("size %d != num_online_cpus %d", size, num_online_cpus());
  for (int i = 0; i < size; i++)
    if (cpulist_parse(cpulists[i], &kstep_masks[type][i]) < 0)
      panic("Failed to parse cpulist %s for level %d", cpulists[i], type);
}

void kstep_topo_set_smt(const char *cpulists[], int size) {
  kstep_topo_set_level(KSTEP_TOPO_SMT, cpulists, size);

  // Also update cpu_sibling_map so that cpu_smt_mask() / is_core_idle()
  // reflect the new SMT topology.
  KSYM_IMPORT_TYPED(cpumask_var_t, cpu_sibling_map);
  for (int i = 0; i < size; i++)
    cpumask_copy(*per_cpu_ptr(KSYM_cpu_sibling_map, i),
                 &kstep_masks[KSTEP_TOPO_SMT][i]);
}

void kstep_topo_set_cls(const char *cpulists[], int size) {
  kstep_topo_set_level(KSTEP_TOPO_CLS, cpulists, size);
}

void kstep_topo_set_mc(const char *cpulists[], int size) {
  kstep_topo_set_level(KSTEP_TOPO_MC, cpulists, size);
}

void kstep_topo_set_pkg(const char *cpulists[], int size) {
  kstep_topo_set_level(KSTEP_TOPO_PKG, cpulists, size);
}

void kstep_topo_set_node(const char *cpulists[], int size) {
  kstep_topo_set_level(KSTEP_TOPO_NODE, cpulists, size);
}

static void kstep_topo_set(const char *name, const char *cpulists[], int size) {
  if (strcmp(name, "SMT") == 0) {
    kstep_topo_set_smt(cpulists, size);
    return;
  }
  if (strcmp(name, "CLS") == 0) {
    kstep_topo_set_cls(cpulists, size);
    return;
  }
  if (strcmp(name, "MC") == 0) {
    kstep_topo_set_mc(cpulists, size);
    return;
  }
  if (strcmp(name, "PKG") == 0) {
    kstep_topo_set_pkg(cpulists, size);
    return;
  }
  if (strcmp(name, "NODE") == 0) {
    kstep_topo_set_node(cpulists, size);
    return;
  }
  panic("Unknown topology level %s", name);
}

void kstep_topo_apply(void) {
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

/*
 * Topology format:
 *   <level>:<cpu-list>/<cpu-list>/... [+ <level>:<cpu-list>/...]
 *
 * Complete example:
 *   "SMT:0/1-2/1-2/3-4/3-4+CLS:0/1-2/1-2/3-4/3-4+PKG:0-4/0-4/0-4/0-4/0-4"
 *
 * This applies the topology levels in order: smt -> cls -> pkg.
 */
bool kstep_topo_param_apply(void) {
  char topo_buf[EXECUTOR_TOPOLOGY_LEN];
  char *cursor, *level_spec;

  if (!executor_topology[0] || strcmp(executor_topology, "none") == 0) {
    pr_info("executor: custom topology disabled\n");
    return false;
  }

  strscpy(topo_buf, executor_topology, sizeof(topo_buf));
  cursor = topo_buf;

  kstep_topo_init();
  while ((level_spec = strsep(&cursor, "+")) != NULL) {
    const char *cpulists[NR_CPUS];
    char *name, *cpulist_spec, *cpulist;
    int nr_cpulists = 0;

    level_spec = strim(level_spec);
    if (!*level_spec)
      continue;

    name = strsep(&level_spec, ":");
    if (!name || !*name || !level_spec || !*level_spec)
      panic("Invalid topology spec %s", executor_topology);

    cpulist_spec = level_spec;
    while ((cpulist = strsep(&cpulist_spec, "/")) != NULL) {
      cpulist = strim(cpulist);
      if (!*cpulist)
        continue;
      if (nr_cpulists >= NR_CPUS)
        panic("Too many cpulists in topology spec %s", executor_topology);
      cpulists[nr_cpulists++] = cpulist;
    }

    if (!nr_cpulists)
      panic("Missing cpulists for topology level %s", name);

    kstep_topo_set(name, cpulists, nr_cpulists);
    TRACE_INFO("executor: applying topology %s (%d cpulists)\n", name, nr_cpulists);
  }

  kstep_topo_apply();
  return true;
}

bool kstep_capacity_param_apply(void) {
  char capacity_buf[EXECUTOR_CAPACITY_LEN];
  char *cursor, *capacity_spec;
  int cpu = 0;

  if (!executor_capacity[0] || strcmp(executor_capacity, "none") == 0) {
    pr_info("executor: custom capacity disabled\n");
    return false;
  }

  strscpy(capacity_buf, executor_capacity, sizeof(capacity_buf));
  cursor = capacity_buf;

  while ((capacity_spec = strsep(&cursor, "/")) != NULL) {
    int scale;

    capacity_spec = strim(capacity_spec);
    if (!*capacity_spec)
      continue;
    if (cpu >= num_online_cpus())
      panic("Too many CPU capacities in spec %s", executor_capacity);

    if (kstrtoint(capacity_spec, 10, &scale) != 0 || scale <= 0)
      panic("Invalid CPU capacity scale %s in spec %s", capacity_spec,
            executor_capacity);

    if (cpu != 0)
      kstep_cpu_set_capacity(cpu, scale);
    TRACE_INFO("executor: applying capacity cpu=%d scale=%d\n", cpu, scale);
    cpu++;
  }

  if (cpu != num_online_cpus())
    panic("Capacity spec %s provided %d CPUs, expected %d", executor_capacity,
          cpu, num_online_cpus());
  return true;
}

void kstep_freq_param_apply(void) {
  char freq_buf[EXECUTOR_FREQUENCY_LEN];
  char *cursor, *freq_spec;
  int cpu = 0;

  if (!executor_frequency[0] || strcmp(executor_frequency, "none") == 0) {
    pr_info("executor: custom frequency disabled\n");
    return;
  }

  strscpy(freq_buf, executor_frequency, sizeof(freq_buf));
  cursor = freq_buf;

  while ((freq_spec = strsep(&cursor, "/")) != NULL) {
    int scale;

    freq_spec = strim(freq_spec);
    if (!*freq_spec)
      continue;
    if (cpu >= num_online_cpus())
      panic("Too many CPU frequencies in spec %s", executor_frequency);
    
    if (kstrtoint(freq_spec, 10, &scale) != 0 || scale <= 0)
      panic("Invalid CPU frequency scale %s in spec %s", freq_spec,
            executor_frequency);

    if (cpu != 0)
      kstep_cpu_set_freq(cpu, scale);
    TRACE_INFO("executor: applying frequency cpu=%d scale=%d\n", cpu, scale);
    cpu++;
  }

  if (cpu != num_online_cpus())
    panic("Frequency spec %s provided %d CPUs, expected %d",
          executor_frequency, cpu, num_online_cpus());
}

void kstep_cpu_set_freq(int cpu, int scale) {
  if (cpu < 0 || cpu >= num_online_cpus())
    panic("cpu %d out of range", cpu);
  // x86:
  // https://elixir.bootlin.com/linux/v6.14.11/source/arch/x86/include/asm/topology.h#L287-L293
  // generic:
  // https://elixir.bootlin.com/linux/v6.14.11/source/include/linux/arch_topology.h#L33-L38
  KSYM_IMPORT(arch_freq_scale);
  *per_cpu_ptr(KSYM_arch_freq_scale, cpu) = scale;
}

void kstep_cpu_set_capacity(int cpu, int scale) {
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
