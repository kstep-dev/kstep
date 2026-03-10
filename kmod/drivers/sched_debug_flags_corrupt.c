// https://github.com/torvalds/linux/commit/8d4d9c7b4333
//
// Bug: sd_ctl_doflags() in kernel/sched/debug.c allocates memory with kcalloc,
// then does pointer arithmetic `tmp += *ppos` for partial reads. On the second
// read (non-zero ppos), kfree(tmp) frees an offset pointer, corrupting the heap.
//
// Reproduce: Read /proc/sys/kernel/sched_domain/cpu0/domain0/flags with partial
// reads. The second read with non-zero ppos triggers kfree on an offset pointer.
// We detect this via an exported counter that fires when kfree arg != original alloc.

#include <linux/version.h>
#include <linux/fs.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 9, 0)

extern int kstep_sd_doflags_offset_count;

static void setup(void) {}

static void run(void) {
  const char *path = "/proc/sys/kernel/sched_domain/cpu0/domain0/flags";
  struct file *file = filp_open(path, O_RDONLY, 0);
  if (IS_ERR(file)) {
    kstep_fail("cannot open %s: %ld", path, PTR_ERR(file));
    return;
  }
  filp_close(file, NULL);

  int before = kstep_sd_doflags_offset_count;

  for (int iter = 0; iter < 5; iter++) {
    file = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(file))
      break;

    char buf[4];
    loff_t pos = 0;
    kernel_read(file, buf, 1, &pos);
    kernel_read(file, buf, 1, &pos);
    filp_close(file, NULL);
  }

  int after = kstep_sd_doflags_offset_count;
  TRACE_INFO("offset_count: before=%d after=%d", before, after);

  if (after > before)
    kstep_fail("kfree called on offset pointer %d times", after - before);
  else
    kstep_pass("no offset kfree detected");
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
