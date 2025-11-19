#include "kstep.h"

static void print_sd_flags(int flags) {
#define SD_FLAG(name, meta_flag)                                               \
  if (flags & name) {                                                          \
    pr_cont("%s, ", &#name[3]);                                                \
  }
#include <linux/sched/sd_flags.h>
#undef SD_FLAG
}

static void print_cpumask(const struct cpumask *mask, int width) {
  static char buf[8];
  snprintf(buf, sizeof(buf), "%*pbl", cpumask_pr_args(mask));
  pr_cont("%*s", width, buf);
}

static void print_topo_levels(void) {
  pr_info("Topology levels:\n");
  for (struct sched_domain_topology_level *tl = *ksym.sched_domain_topology;
       tl->mask; tl++) {
    pr_info("- %-5s| ", tl->name);
    for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
      print_cpumask(tl->mask(cpu), 4);
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
  for (struct sched_domain_topology_level *tl = *ksym.sched_domain_topology;
       tl->mask; tl++) {
    for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
      struct sched_domain *sd;
      for_each_domain(cpu, sd) {
        if (strcmp(sd->name, tl->name) == 0) {
          pr_info("- %s[%d]: ", tl->name, cpu);
          print_sched_domain(sd);
        }
      }
    }
  }
}

static void update_topology(void) {
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

void kstep_topo_print(void) {
  print_topo_levels();
  print_sched_domains();
}

void kstep_set_cpu_freq(int cpu, int scale) {
  // x86:
  // https://elixir.bootlin.com/linux/v6.14.11/source/arch/x86/include/asm/topology.h#L287-L293
  // generic:
  // https://elixir.bootlin.com/linux/v6.14.11/source/include/linux/arch_topology.h#L33-L38
  *per_cpu_ptr(ksym.arch_freq_scale, cpu) = scale;
}

void kstep_set_cpu_capacity(int cpu, int scale) {
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

void kstep_use_special_topo(void) {
  for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
    kstep_set_cpu_capacity(cpu, (cpu % 2 == 0) ? SCHED_CAPACITY_SCALE
                                               : SCHED_CAPACITY_SCALE >> 1);
  }

  // Qemu x86 does not support cluster CPU topology, simulate with die (i.e.,
  // PKG) using cluster flags.
  for (struct sched_domain_topology_level *tl = *ksym.sched_domain_topology;
       tl->mask; tl++) {
    if (strcmp(tl->name, "PKG") == 0) {
      tl->sd_flags = ksym.cpu_cluster_flags;
    }
  }
  update_topology();
}
