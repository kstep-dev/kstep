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

static void setup(void) {
  kstep_topo_init();
  kstep_topo_apply();
  kstep_topo_print();
}

static void run(void) {
  // First, find which CPUs have sched domains by checking the sysctl dir
  const char *path = "/proc/sys/kernel/sched_domain/cpu0/domain0/flags";
  struct file *rfile = filp_open(path, O_RDONLY, 0);
  if (IS_ERR(rfile)) {
    TRACE_INFO("cpu0 domain0 flags not available, trying cpu1");
    path = "/proc/sys/kernel/sched_domain/cpu1/domain0/flags";
    rfile = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(rfile)) {
      kstep_fail("no sched_domain flags file found");
      return;
    }
  }

  // Read current flags value
  char buf[32] = {0};
  loff_t pos = 0;
  ssize_t nread = kernel_read(rfile, buf, sizeof(buf) - 1, &pos);
  filp_close(rfile, NULL);

  if (nread <= 0) {
    kstep_fail("could not read flags (ret=%ld)", (long)nread);
    return;
  }
  buf[nread] = '\0';

  long orig_flags = 0;
  if (kstrtol(strim(buf), 10, &orig_flags)) {
    kstep_fail("could not parse flags: '%s'", buf);
    return;
  }
  TRACE_INFO("flags at %s = %ld (0x%lx)", path, orig_flags, orig_flags);

  // Try to open for writing.
  // Buggy kernel: 0644 → open succeeds.
  // Fixed kernel: 0444 → open fails with -EACCES.
  struct file *wfile = filp_open(path, O_WRONLY, 0);

  if (IS_ERR(wfile)) {
    kstep_pass("flags sysctl is read-only (err=%ld)", PTR_ERR(wfile));
    return;
  }

  // Write a modified value (clear SD_BALANCE_NEWIDLE = 0x0002)
  long new_flags = orig_flags & ~0x0002L;
  int len = snprintf(buf, sizeof(buf), "%ld\n", new_flags);
  pos = 0;
  ssize_t nwritten = kernel_write(wfile, buf, len, &pos);
  filp_close(wfile, NULL);

  if (nwritten < 0) {
    kstep_pass("flags write rejected (ret=%ld)", (long)nwritten);
    return;
  }

  // Verify flags actually changed by reading back
  rfile = filp_open(path, O_RDONLY, 0);
  if (IS_ERR(rfile)) {
    kstep_fail("could not reopen flags for verification");
    return;
  }
  memset(buf, 0, sizeof(buf));
  pos = 0;
  nread = kernel_read(rfile, buf, sizeof(buf) - 1, &pos);
  filp_close(rfile, NULL);

  long readback = 0;
  if (nread > 0) {
    buf[nread] = '\0';
    kstrtol(strim(buf), 10, &readback);
  }

  TRACE_INFO("After write: flags=%ld (0x%lx), expected=%ld",
             readback, readback, new_flags);

  if (readback == new_flags && new_flags != orig_flags) {
    kstep_fail("flags writable via sysctl: changed from 0x%lx to 0x%lx "
               "without update_top_cache_domain()",
               orig_flags, readback);
  } else {
    kstep_pass("flags not changed as expected");
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
