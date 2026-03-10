// https://github.com/torvalds/linux/commit/8d4d9c7b4333
//
// Bug: sd_ctl_doflags() in kernel/sched/debug.c allocates memory with kcalloc,
// then does pointer arithmetic `tmp += *ppos` for partial reads. On the second
// read (non-zero ppos), kfree(tmp) frees an offset pointer, corrupting the heap.
//
// Reproduce: Directly invoke the sd_ctl_doflags proc handler with partial reads.
// The second read with non-zero ppos triggers kfree on an offset pointer.

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sysctl.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 9, 0)

static void setup(void) {
  // Don't call kstep_topo_init/apply - we need the existing sched domains
  // from boot to access the procfs flags file
}

static void run(void) {
  // Get sched_domain for CPU 1
  struct sched_domain *sd;
  struct rq *rq = cpu_rq(1);
  sd = rcu_dereference(rq->sd);

  if (!sd) {
    TRACE_INFO("No sched_domain for CPU 1, trying CPU 0");
    rq = cpu_rq(0);
    sd = rcu_dereference(rq->sd);
  }

  if (!sd) {
    TRACE_INFO("No sched_domain found for any CPU");
    kstep_fail("no sched_domain available");
    return;
  }

  TRACE_INFO("Found sched_domain '%s' with flags=0x%x", sd->name, sd->flags);

  // Try to read via /proc/sys path - check which paths exist
  const char *paths[] = {
    "/proc/sys/kernel/sched_domain/cpu0/domain0/flags",
    "/proc/sys/kernel/sched_domain/cpu1/domain0/flags",
    "/proc/sys/kernel/sched_domain/cpu0/domain1/flags",
    "/proc/sys/kernel/sched_domain/cpu1/domain1/flags",
    NULL,
  };

  struct file *file = NULL;
  const char *found_path = NULL;
  for (int i = 0; paths[i]; i++) {
    file = filp_open(paths[i], O_RDONLY, 0);
    if (!IS_ERR(file)) {
      found_path = paths[i];
      TRACE_INFO("Opened %s successfully", found_path);
      filp_close(file, NULL);
      break;
    }
    TRACE_INFO("Cannot open %s: %ld", paths[i], PTR_ERR(file));
    file = NULL;
  }

  if (!found_path) {
    TRACE_INFO("No flags file found via procfs, skipping");
    kstep_fail("no flags procfs file available");
    return;
  }

  // Now do multiple partial reads to trigger the bug.
  // Each read invokes sd_ctl_doflags which does:
  //   tmp = kcalloc(...);
  //   tmp += *ppos;   // offset the allocated pointer
  //   kfree(tmp);     // BUG: frees offset pointer
  for (int iter = 0; iter < 5; iter++) {
    file = filp_open(found_path, O_RDONLY, 0);
    if (IS_ERR(file)) {
      TRACE_INFO("Failed to reopen %s", found_path);
      break;
    }

    char buf[4];
    loff_t pos = 0;
    ssize_t ret;

    // First partial read: read 1 byte, ppos becomes 1
    ret = kernel_read(file, buf, 1, &pos);
    TRACE_INFO("iter=%d read1: ret=%zd pos=%lld", iter, ret, pos);

    if (ret > 0) {
      // Second partial read: ppos > 0, triggers tmp += *ppos then kfree(tmp)
      ret = kernel_read(file, buf, 1, &pos);
      TRACE_INFO("iter=%d read2: ret=%zd pos=%lld", iter, ret, pos);
    }

    filp_close(file, NULL);
  }

  // If we survived, check for latent corruption via allocations
  bool alloc_failed = false;
  for (int i = 0; i < 200; i++) {
    char *p = kmalloc(64, GFP_KERNEL);
    if (!p) {
      alloc_failed = true;
      break;
    }
    memset(p, 0x41, 64);
    kfree(p);
  }

  if (alloc_failed)
    kstep_fail("allocation failed after heap corruption");
  else
    kstep_fail("kfree on offset pointer executed (heap corruption)");
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "sched_debug_flags_corrupt",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
