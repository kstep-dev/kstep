#include <linux/arch_topology.h>
#include <linux/moduleparam.h>
#include <linux/cpuset.h>

#include "driver.h"
#include "internal.h"

KSYM_IMPORT_TYPED(struct sched_domain_topology_level *, sched_domain_topology);
#define for_each_tl(tl) for (tl = *KSYM_sched_domain_topology; tl->mask; tl++)

// The set SD_* flag names, comma separated, from the kernel's own sd_flags.h -- the one place that
// knows them. Taking them from the kernel rather than a table on the host means neither the log
// nor the page can drift from the kernel in front of them (SD_SHARE_PKG_RESOURCES became
// SD_SHARE_LLC, and such renames are routine). A list too long for the buffer is truncated.
void kstep_sd_flags_str(int flags, char *buf, size_t len) {
  size_t n = 0;

  buf[0] = '\0';
#define SD_FLAG(name, meta_flag)                                               \
  if (flags & name)                                                            \
    n += scnprintf(buf + n, len - n, "%s%s", n ? ", " : "", &#name[3]);
#include <linux/sched/sd_flags.h>
#undef SD_FLAG
}

// 160: the longest list the levels below produce needs about 140.
static void print_sd_flags(int flags) {
  char buf[160];

  kstep_sd_flags_str(flags, buf, sizeof(buf));
  pr_cont("%s", buf);
}

static void print_cpumask(const struct cpumask *mask, int width) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%*pbl", cpumask_pr_args(mask));
  pr_cont("%*s", width, buf);
}

static const struct cpumask *topo_mask(struct sched_domain_topology_level *tl,
                                      int cpu) {
// https://github.com/torvalds/linux/commit/661f951e371cc134ea31c84238dbdc9a898b8403
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  return tl->mask(tl, cpu);
#else
  return tl->mask(cpu);
#endif
}

static void print_topo_levels(void) {
  pr_info("Topology levels:\n");
  struct sched_domain_topology_level *tl;
  for_each_tl(tl) {
    pr_info("- %-5s| ", tl->name);
    for_each_test_cpu(cpu) {
      const struct cpumask *mask = topo_mask(tl, cpu);
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
  for_each_tl(tl) {
    for_each_test_cpu(cpu) {
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

// https://github.com/torvalds/linux/commit/f577cd57bfaa889cf0718e30e92c08c7f78c9d85 (DIE -> PKG)
static const char *const topo_level_names[KSTEP_TOPO_NR] = {"SMT", "CLS", "MC", "PKG", "NODE"};
static int get_topo_level(const char *name) {
  for (int level = 0; level < KSTEP_TOPO_NR; level++)
    if (strcmp(name, topo_level_names[level]) == 0)
      return level;
  return strcmp(name, "DIE") == 0 ? KSTEP_TOPO_PKG : -1;
}

int kstep_test_ncpus;

// The CPUs the module works with: as many as it has room for, every one but the controller's a
// test CPU, on the machine's own topology with CPU 0 isolated -- until a driver's cpu-topo spec
// says otherwise.
void kstep_cpu_init(void) {
  if (num_online_cpus() > KSTEP_NR_CPUS)
    panic("Number of online CPUs (%d) exceeds KSTEP_NR_CPUS (%d)", num_online_cpus(), KSTEP_NR_CPUS);
  kstep_test_ncpus = num_online_cpus() - 1;
  kstep_topo_set("");
}

// The machine's own masks over the test CPUs, with CPU 0 and every spare CPU alone at every level,
// so the test CPUs' load balancers never read the controller's non-deterministic state and never
// see a spare: a single-CPU level degenerates, leaving them on a null domain, like an offline CPU
// would be without the hotplug the mocked clock cannot do. The kernel's topology table is pointed
// at our masks.
static void topo_init(void) {
  struct sched_domain_topology_level *tl;
  struct cpumask test;

  cpumask_clear(&test);
  for_each_test_cpu(cpu)
    cpumask_set_cpu(cpu, &test);
  for_each_tl(tl) {
    int level = get_topo_level(tl->name);
    if (level < 0)
      panic("Unknown topology level %s", tl->name);
    for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
      struct cpumask *m = &kstep_masks[level][cpu];
      if (!cpumask_test_cpu(cpu, &test)) {
        cpumask_copy(m, cpumask_of(cpu));
        continue;
      }
      cpumask_and(m, topo_mask(tl, cpu), &test);
    }
    tl->mask = kstep_masks_fns[level];
  }
}


// ---- the topology spec ----
//
//   KEY=group|group;KEY=group|group...     a group is a cpulist; CAP groups are cpulist:scale
//
// A key is a level (SMT, CLS, MC, PKG, NODE), CAP, or CPUS. CPUS=K makes 1..K the test CPUs (all
// of 1..N-1 when absent); the rest of the machine is spare, idle and unseen, so one boot serves
// every smaller machine. At a level named, the CPUs of a group share it and a CPU not named is
// alone; a level not named keeps the machine's own, and one this kernel does not have goes unused. Lower levels must nest in higher ones: the kernel says so itself
// ("arch topology borken") and widens the parent. CAP is the capacity, 1..1024, of the CPUs
// named, 1024 for the rest. CPU 0 is the controller's, is never named and stays alone at every
// level.
//
//   CLS=1-2|3-4;CAP=2,4:512      two clusters, a little core in each (even_idle_cpu)
//   CPUS=2                       CPUs 1 and 2 of whatever the machine has; the others idle

// Capacity is domain-building state as well as a per-CPU value: SD_ASYM_CPUCAPACITY, the
// sched_asym_cpucapacity key and rd->max_cpu_capacity are computed only when the domains are
// built. So it is part of the topology spec and set by kstep_topo_set before its one rebuild.
static void set_cap(int cpu, int scale) {
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
  // x86 before 6.12: every CPU has SCHED_CAPACITY_SCALE and nothing can change it. The default
  // spec asks for exactly that, so only a spec naming another capacity is refused.
  if (scale != SCHED_CAPACITY_SCALE)
    panic("Setting CPU capacity is not supported for this kernel");
#endif
}

// A nonempty cpulist ("1", "1-3", "1,3") of test CPUs: within 1..K, never CPU 0, the controller's.
bool kstep_parse_cpus(const char *list, struct cpumask *mask) {
  return list && !cpulist_parse(list, mask) && !cpumask_empty(mask) && !cpumask_test_cpu(0, mask) &&
         cpumask_last(mask) <= kstep_test_ncpus;
}

// One group's cpulist: test CPUs, each named once per key.
static const char *topo_parse_group(const char *list, struct cpumask *group, struct cpumask *seen) {
  if (!kstep_parse_cpus(list, group))
    return "bad cpulist: test CPUs 1..CPUS only";
  if (cpumask_intersects(group, seen))
    return "a CPU is named twice under one key";
  cpumask_or(seen, seen, group);
  return NULL;
}

// One key: a level's groups into its masks, or CAP's cpulist:scale groups into caps[].
static const char *topo_set_key(const char *key, char *groups, int *caps) {
  bool cap = strcmp(key, "CAP") == 0;
  int level = cap ? 0 : get_topo_level(key), cpu, scale = 0;
  struct cpumask seen, group;
  char *list;

  if (strcmp(key, "CPUS") == 0)
    return NULL; // read ahead of the levels, by kstep_topo_cpus
  if (level < 0)
    return "unknown key (a level, CAP or CPUS)";
  if (!cap) // alone unless named
    for_each_test_cpu(cpu)
      cpumask_copy(&kstep_masks[level][cpu], cpumask_of(cpu));
  cpumask_clear(&seen);
  while ((list = strsep(&groups, "|")) != NULL) {
    if (cap) {
      char *val = strchr(list, ':');
      if (!val)
        return "CAP groups are cpulist:scale";
      *val++ = '\0';
      if (kstrtoint(val, 10, &scale) || scale < 1 || scale > SCHED_CAPACITY_SCALE)
        return "capacity is not in 1..1024";
    }
    const char *err = topo_parse_group(list, &group, &seen);
    if (err)
      return err;
    for_each_cpu(cpu, &group)
      if (cap)
        caps[cpu] = scale;
      else
        cpumask_copy(&kstep_masks[level][cpu], &group);
  }
  return NULL;
}

// The test CPU count a spec asks for: CPUS=K within 1..N-1, or N-1 when it says nothing; -1 for
// a malformed value. Read ahead of the rest of the spec, since the levels are bounded by it.
static int topo_cpus(const char *spec) {
  char buf[CPU_SPEC_LEN];
  char *cursor = buf, *entry;
  int n = num_online_cpus() - 1;

  strscpy(buf, spec, sizeof(buf));
  while ((entry = strsep(&cursor, ";")) != NULL) {
    if (!strstarts(entry, "CPUS="))
      continue;
    if (kstrtoint(entry + 5, 10, &n) || n < 1 || n > num_online_cpus() - 1)
      return -1;
  }
  return n;
}

// The fair class scales its slice by the machine's size at boot: sysctl_sched_base_slice =
// normalized_sysctl_sched_base_slice * (1 + ilog2(min(num_online_cpus(), 8))), from update_sysctl
// and get_update_sysctl_factor in kernel/sched/fair.c (SCHED_TUNABLESCALING_LOG, the default,
// unchanged since 2.6.32). A layout resumed from a larger boot would otherwise run a longer slice
// than the same layout booted cold, so the value is redone for the test CPUs plus the controller.
// After the domain rebuild, which attaches the runqueues to their root domain and reruns the
// kernel's own scaling (rq_online_fair). The one EEVDF tunable: the page's image is v6.18. The
// boot's value is checked against the formula first, so a kernel that scales differently is
// refused rather than given a wrong slice.
static unsigned int slice_factor(unsigned int cpus) { return 1 + ilog2(min_t(unsigned int, cpus, 8)); }
static void set_slice_scaling(void) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  KSYM_IMPORT(sysctl_sched_base_slice);
  KSYM_IMPORT_TYPED(unsigned int, normalized_sysctl_sched_base_slice);
  static bool checked;
  if (!checked && *KSYM_sysctl_sched_base_slice != *KSYM_normalized_sysctl_sched_base_slice * slice_factor(num_online_cpus()))
    panic("sysctl_sched_base_slice %u is not normalized %u * factor(%d): this kernel scales the slice differently",
          *KSYM_sysctl_sched_base_slice, *KSYM_normalized_sysctl_sched_base_slice, num_online_cpus());
  checked = true;
  *KSYM_sysctl_sched_base_slice = *KSYM_normalized_sysctl_sched_base_slice * slice_factor(kstep_test_ncpus + 1);
#endif
}

// NULL once applied, or what is wrong with the spec, which then changes nothing the kernel
// reads: masks are read at the rebuild and capacities are set last. Before the first task: a
// task's affinity and a cgroup's cpuset are drawn from CPUS, and a task left allowed on a CPU
// that a later spec made spare would run there unticked and unwatched. "" is the machine's own
// topology with CPU 0 isolated and every CPU a test CPU.
const char *kstep_topo_set(const char *spec) {
  char buf[CPU_SPEC_LEN];
  char *cursor = buf, *entry;
  int caps[KSTEP_NR_CPUS] = {[0 ... KSTEP_NR_CPUS - 1] = SCHED_CAPACITY_SCALE}, cpu;
  int n = topo_cpus(spec);

  if (n < 0)
    return "CPUS is not in 1..N-1";
  kstep_test_ncpus = n;
  strscpy(buf, spec, sizeof(buf));
  topo_init();
  while ((entry = strsep(&cursor, ";")) != NULL) {
    if (!*entry)
      continue;
    char *key = strsep(&entry, "=");
    if (!entry || !*entry)
      return "expected KEY=group|group";
    const char *err = topo_set_key(key, entry, caps);
    if (err)
      return err;
  }
  for_each_test_cpu(c)
    set_cap(c, caps[c]);

  // The arch's own topology state, then the rebuild. The sibling masks feed cpu_smt_mask() /
  // is_core_idle() directly, bypassing the topology table, so they must say the same as SMT.
#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
  // https://elixir.bootlin.com/linux/v6.17.8/source/include/linux/arch_topology.h#L88
  KSYM_IMPORT_TYPED(struct cpu_topology, cpu_topology);
  for_each_cpu(cpu, cpu_online_mask)
    cpumask_copy(&KSYM_cpu_topology[cpu].thread_sibling, &kstep_masks[KSTEP_TOPO_SMT][cpu]);
  // https://elixir.bootlin.com/linux/v6.17.8/source/drivers/base/arch_topology.c#L205-L222
  KSYM_IMPORT_TYPED(int, update_topology);
  *KSYM_update_topology = 1; // else a rebuild with unchanged cpumasks is skipped as a no-op
#else
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/smpboot.c#L95-L96
  KSYM_IMPORT_TYPED(cpumask_var_t, cpu_sibling_map);
  for_each_cpu(cpu, cpu_online_mask)
    cpumask_copy(*per_cpu_ptr(KSYM_cpu_sibling_map, cpu), &kstep_masks[KSTEP_TOPO_SMT][cpu]);
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/itmt.c#L55-L56
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/smpboot.c#L129-L138
  KSYM_IMPORT(x86_topology_update);
  *KSYM_x86_topology_update = true;
#endif

  KSYM_IMPORT(rebuild_sched_domains);
  KSYM_rebuild_sched_domains();
  set_slice_scaling();
  return NULL;
}

// The kernel's arch_scale_freq_capacity() reads this per-CPU value, but the symbol is not exported.
// x86: https://elixir.bootlin.com/linux/v6.14.11/source/arch/x86/include/asm/topology.h#L287-L293
// generic: https://elixir.bootlin.com/linux/v6.14.11/source/include/linux/arch_topology.h#L33-L38
KSYM_IMPORT(arch_freq_scale);
void kstep_freq_set(int cpu, int scale) { *per_cpu_ptr(KSYM_arch_freq_scale, cpu) = scale; }
unsigned long kstep_freq_get(int cpu) { return *per_cpu_ptr(KSYM_arch_freq_scale, cpu); }


static void print_capacities(void) {
  pr_info("CPU capacities:\n");
  for_each_test_cpu(cpu)
    pr_info("- CPU %d: %lu\n", cpu, arch_scale_cpu_capacity(cpu));
}

void kstep_topo_print(void) {
  print_topo_levels();
  print_sched_domains();
  print_capacities();
}
