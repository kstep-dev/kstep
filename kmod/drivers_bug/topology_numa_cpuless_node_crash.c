// Reproduce: sched_numa_hop_mask() returns NULL for CPU-less NUMA nodes
// instead of ERR_PTR, violating its documented API contract.
// Reference: 617f2c38cb5c (original fix for sched_numa_find_nth_cpu)
//
// Strategy: On a UMA system there are no CPU-less nodes, so we simulate
// one by temporarily NULLing out an entry in sched_domains_numa_masks.
// This is exactly what sched_init_numa() leaves behind for CPU-less
// nodes (it never initializes their mask entries).

#include "driver.h"
#include "internal.h"
#include <linux/nodemask.h>
#include <linux/topology.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

KSYM_IMPORT_TYPED(struct cpumask ***, sched_domains_numa_masks);
KSYM_IMPORT_TYPED(int, sched_domains_numa_levels);

static void setup(void) {}

static void run(void) {
  int target_node = 0; // Use node 0 — guaranteed to exist
  const struct cpumask *mask;
  struct cpumask *saved_mask;
  struct cpumask ***masks;
  int levels;

  levels = *KSYM_sched_domains_numa_levels;
  TRACE_INFO("nr_node_ids=%u, numa_levels=%d, num_online_cpus=%u",
             nr_node_ids, levels, num_online_cpus());

  if (levels < 1) {
    kstep_pass("No NUMA levels configured (UMA without NUMA distances)");
    return;
  }

  rcu_read_lock();
  masks = rcu_dereference(*KSYM_sched_domains_numa_masks);
  if (!masks) {
    rcu_read_unlock();
    kstep_fail("sched_domains_numa_masks is NULL");
    return;
  }

  // Save the real mask for node 0 at hop 0, then NULL it out to
  // simulate what sched_init_numa() leaves for CPU-less nodes.
  saved_mask = masks[0][target_node];
  TRACE_INFO("Original masks[0][%d] = %px (cpus: %*pbl)",
             target_node, saved_mask, cpumask_pr_args(saved_mask));

  masks[0][target_node] = NULL;
  TRACE_INFO("Set masks[0][%d] = NULL (simulating CPU-less node)",
             target_node);

  // Call the function under test — on buggy kernel this returns NULL,
  // on fixed kernel numa_nearest_node() redirects before the lookup.
  mask = sched_numa_hop_mask(target_node, 0);

  // Restore immediately
  masks[0][target_node] = saved_mask;
  rcu_read_unlock();

  TRACE_INFO("sched_numa_hop_mask(%d, 0) returned %px "
             "(IS_ERR=%d, IS_NULL=%d)",
             target_node, mask, IS_ERR(mask), mask == NULL);

  if (mask == NULL) {
    kstep_fail("sched_numa_hop_mask(%d, 0) returned NULL for "
               "CPU-less node (violates ERR_PTR API contract)",
               target_node);
  } else if (IS_ERR(mask)) {
    TRACE_INFO("Returned ERR_PTR(%ld)", PTR_ERR(mask));
    kstep_pass("sched_numa_hop_mask correctly returned ERR_PTR "
               "for CPU-less node %d",
               target_node);
  } else {
    TRACE_INFO("Returned valid mask: %*pbl", cpumask_pr_args(mask));
    kstep_pass("sched_numa_hop_mask returned valid mask for "
               "CPU-less node %d (redirected to nearest CPU node)",
               target_node);
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
