// Reproduce: sched_numa_hop_mask() returns NULL for CPU-less NUMA nodes
// instead of ERR_PTR, violating its documented API contract.
// Reference: 617f2c38cb5c (original fix for sched_numa_find_nth_cpu)

#include "driver.h"
#include "internal.h"
#include <linux/nodemask.h>
#include <linux/topology.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

static void setup(void) {}

static void run(void) {
  int node;
  int cpuless_node = -1;
  const struct cpumask *mask;

  TRACE_INFO("nr_node_ids=%u, nr_online_nodes=%u, num_online_cpus=%u",
             nr_node_ids, num_online_nodes(), num_online_cpus());

  for_each_online_node(node) {
    bool has_cpus = node_state(node, N_CPU);
    TRACE_INFO("node %d: online=yes, has_cpus=%s", node,
               has_cpus ? "yes" : "no");
    if (!has_cpus && cpuless_node < 0)
      cpuless_node = node;
  }

  if (cpuless_node < 0) {
    TRACE_INFO("No CPU-less NUMA nodes found, cannot test bug");
    kstep_pass("No CPU-less nodes available to test");
    return;
  }

  TRACE_INFO("Testing sched_numa_hop_mask(%d, 0) on CPU-less node",
             cpuless_node);

  rcu_read_lock();
  mask = sched_numa_hop_mask(cpuless_node, 0);
  rcu_read_unlock();

  TRACE_INFO("sched_numa_hop_mask(%d, 0) returned %px "
             "(IS_ERR=%d, IS_NULL=%d)",
             cpuless_node, mask, IS_ERR(mask), mask == NULL);

  if (mask == NULL) {
    kstep_fail("sched_numa_hop_mask(%d, 0) returned NULL for "
               "CPU-less node (violates ERR_PTR API contract)",
               cpuless_node);
  } else if (IS_ERR(mask)) {
    TRACE_INFO("Returned ERR_PTR(%ld)", PTR_ERR(mask));
    kstep_pass("sched_numa_hop_mask correctly returned ERR_PTR "
               "for CPU-less node %d",
               cpuless_node);
  } else {
    TRACE_INFO("Returned valid mask: %*pbl", cpumask_pr_args(mask));
    kstep_pass("sched_numa_hop_mask returned valid mask for "
               "CPU-less node %d (redirected to nearest CPU node)",
               cpuless_node);
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "topology_numa_cpuless_node_crash",
    .setup = setup,
    .run = run,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("Kernel too old"); }
KSTEP_DRIVER_DEFINE{
    .name = "topology_numa_cpuless_node_crash",
    .setup = setup,
    .run = run,
};
#endif
