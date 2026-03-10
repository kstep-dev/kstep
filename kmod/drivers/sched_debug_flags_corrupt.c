// https://github.com/torvalds/linux/commit/8d4d9c7b4333
//
// Bug: sd_ctl_doflags() in kernel/sched/debug.c allocates memory with kcalloc,
// then does pointer arithmetic `tmp += *ppos` for partial reads. On the second
// read (non-zero ppos), kfree(tmp) frees an offset pointer, corrupting the heap.
//
// Reproduce: With slub_debug=FZ boot param, partial reads of the sched domain
// flags file trigger SLUB corruption detection when kfree frees the offset pointer.
// We detect the SLUB error via the TAINT_BAD_PAGE kernel taint flag.

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 9, 0)

static bool check_slub_error(void) {
  unsigned long *taint = (unsigned long *)kstep_ksym_lookup("tainted_mask");
  if (!taint)
    return false;
  return test_bit(TAINT_BAD_PAGE, taint);
}

static void setup(void) {}

static void run(void) {
  const char *path = "/proc/sys/kernel/sched_domain/cpu0/domain0/flags";
  struct file *file = filp_open(path, O_RDONLY, 0);
  if (IS_ERR(file)) {
    kstep_fail("cannot open %s: %ld", path, PTR_ERR(file));
    return;
  }
  filp_close(file, NULL);

  bool tainted_before = check_slub_error();

  // Partial reads trigger sd_ctl_doflags with non-zero *ppos.
  // On buggy kernel: tmp += *ppos then kfree(tmp) frees offset pointer.
  // With slub_debug=FZ, SLUB detects the invalid free and sets TAINT_BAD_PAGE.
  for (int iter = 0; iter < 10; iter++) {
    file = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(file))
      break;

    char buf[4];
    loff_t pos = 0;
    kernel_read(file, buf, 1, &pos);
    kernel_read(file, buf, 1, &pos);
    filp_close(file, NULL);
  }

  bool tainted_after = check_slub_error();

  if (!tainted_before && tainted_after)
    kstep_fail("SLUB detected kfree on offset pointer in sd_ctl_doflags");
  else if (tainted_before)
    kstep_fail("kernel already tainted before test");
  else
    kstep_pass("no heap corruption detected");
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
