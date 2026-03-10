// https://github.com/torvalds/linux/commit/9818427c6270
//
// Bug: sd->flags sysctl is writable (mode 0644), but writing to it directly
// modifies sd->flags without calling update_top_cache_domain(). This causes
// cached per-cpu domain pointers (sd_llc, sd_numa, etc.) to become stale,
// as they still reference domains based on the old flag values.
//
// Fix: Make the flags sysctl read-only (mode 0444).
//
// Reproduce: After sched_domain topology is built, open the flags sysctl file
// for writing. On buggy kernel the open succeeds (0644); we then write modified
// flags and verify that sd_llc becomes stale. On fixed kernel the open fails
// (0444 = read-only), keeping flags consistent.

#include <linux/version.h>
#include <linux/fs.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 7, 0)

KSYM_IMPORT(sd_llc);

static void setup(void) {
  kstep_topo_init();
  const char *mc[] = {"1-2"};
  kstep_topo_set_mc(mc, 1);
  kstep_topo_apply();
}

static void run(void) {
  int cpu = 1;

  struct sched_domain *cached = rcu_dereference_check(
      per_cpu(*KSYM_sd_llc, cpu), true);
  if (!cached) {
    kstep_fail("sd_llc is NULL for cpu %d", cpu);
    return;
  }

  int orig_flags = cached->flags;
  TRACE_INFO("sd_llc[%d] flags=0x%x", cpu, orig_flags);

  if (!(orig_flags & 0x0200)) { // SD_SHARE_PKG_RESOURCES = 0x0200
    kstep_fail("sd_llc missing SD_SHARE_PKG_RESOURCES");
    return;
  }

  // Try to open the flags sysctl for writing.
  // Buggy kernel: 0644 → open succeeds.
  // Fixed kernel: 0444 → open fails with -EACCES.
  const char *path = "/proc/sys/kernel/sched_domain/cpu1/domain0/flags";
  struct file *file = filp_open(path, O_WRONLY, 0);

  if (IS_ERR(file)) {
    // Fixed kernel: write denied
    kstep_pass("flags sysctl is read-only (open err=%ld)", PTR_ERR(file));
    return;
  }

  // Buggy kernel: write allowed — clear SD_SHARE_PKG_RESOURCES
  int new_flags = orig_flags & ~0x0200;
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%d\n", new_flags);

  loff_t pos = 0;
  ssize_t ret = kernel_write(file, buf, len, &pos);
  filp_close(file, NULL);

  if (ret < 0) {
    kstep_pass("flags write rejected (ret=%ld)", (long)ret);
    return;
  }

  TRACE_INFO("Wrote flags 0x%x (removed SD_SHARE_PKG_RESOURCES)", new_flags);

  // Verify: sd_llc should still point to the same domain (stale!)
  struct sched_domain *after = rcu_dereference_check(
      per_cpu(*KSYM_sd_llc, cpu), true);
  int after_flags = after ? after->flags : -1;

  TRACE_INFO("After write: sd_llc[%d] flags=0x%x", cpu, after_flags);

  // Walk domains explicitly to find SD_SHARE_PKG_RESOURCES
  struct sched_domain *sd;
  bool walk_found = false;
  rcu_read_lock();
  for_each_domain(cpu, sd) {
    if (sd->flags & 0x0200) {
      walk_found = true;
      break;
    }
  }
  rcu_read_unlock();

  TRACE_INFO("Domain walk SD_SHARE_PKG_RESOURCES: %s",
             walk_found ? "found" : "NOT found");

  // The bug: cached sd_llc still exists, but the domain it points to
  // no longer has SD_SHARE_PKG_RESOURCES. This stale pointer means
  // code using sd_llc will make decisions based on outdated topology.
  if (after && !(after->flags & 0x0200)) {
    kstep_fail("stale sd_llc: cached domain flags=0x%x, "
               "SD_SHARE_PKG_RESOURCES cleared but pointer not updated",
               after_flags);
  } else {
    kstep_pass("no stale sd_llc detected");
  }
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "sd_flags_readonly",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
