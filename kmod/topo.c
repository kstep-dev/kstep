#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/cpumask.h>
#include <linux/find.h>

#include "kstep.h"

static char *sd_flags_to_str(int flags) {
  static char str[256];
  str[0] = '\0';
#define SD_FLAG(name, meta_flag)                                               \
  if (flags & name) {                                                          \
    strlcat(str, #name " | ", sizeof(str));                                    \
  }
#include <linux/sched/sd_flags.h>
#undef SD_FLAG
  if (strlen(str) > 3) { // remove the last " | "
    str[strlen(str) - 3] = '\0';
  }
  return str;
}

static void print_topo_levels(void) {
  TRACE_DEBUG("Topology levels:");
  for (struct sched_domain_topology_level *tl = *ksym.sched_domain_topology;
       tl->mask; tl++) {
    TRACE_DEBUG("- %-5s, flags: %s", tl->name,
                sd_flags_to_str(tl->sd_flags ? (tl->sd_flags()) : 0));
  }
}

static void print_sched_domains(void) {
  TRACE_DEBUG("Sched domains:");
  for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    struct sched_domain *sd;
    for_each_domain(cpu, sd) {
      TRACE_DEBUG("- CPU %d: %-5s, span: %*pbl, group_capacity: %ld, flags: %s", cpu, sd->name,
                  cpumask_pr_args(sched_domain_span(sd)), 
                  sd->groups->sgc->capacity,
                  sd_flags_to_str(sd->flags));
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

void kstep_use_special_topo(void) {
  // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/cpu/aperfmperf.c#L362-L393
  ksym.arch_enable_hybrid_capacity_scale();
  for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
    int cap = (cpu % 2 == 0) ? 100 : 50;
    // https://elixir.bootlin.com/linux/v6.17.8/source/arch/x86/kernel/cpu/aperfmperf.c#L395-L422
    ksym.arch_set_cpu_capacity(cpu, cap, 100, cap, 100);
  }

  // Qemu x86 does not support cluster CPU topology, simulate with die (i.e.,
  // PKG) using cluster flags.
  for (struct sched_domain_topology_level *tl = *ksym.sched_domain_topology;
       tl->mask; tl++) {
    if (strcmp(tl->name, "PKG") == 0) {
      tl->sd_flags = cpu_cluster_flags;
    }
  }
  update_topology();
  kstep_topo_print();
}
