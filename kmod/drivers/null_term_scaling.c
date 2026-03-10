// https://github.com/torvalds/linux/commit/703066188f63
//
// Bug: sched_scaling_write() has two issues:
// 1. Fails to null-terminate the buffer before calling kstrtouint()
// 2. Missing range validation allows out-of-range values to be accepted
//
// We reproduce the range validation bug: writing an invalid value (3) succeeds
// on the buggy kernel (no range check) but correctly fails on the fixed kernel.

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 15, 0)

static void setup(void) {}

// Write to a debugfs file that uses copy_from_user in its .write handler.
// Works because module_init runs in PID 1 context which already has a user mm.
static ssize_t debugfs_try_write(const char *path, const char *data,
                                 size_t len) {
  struct file *file = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(file))
    return PTR_ERR(file);

  if (!file->f_op || !file->f_op->write) {
    filp_close(file, NULL);
    return -ENOSYS;
  }

  unsigned long uaddr =
      vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
              MAP_ANONYMOUS | MAP_PRIVATE, 0);
  ssize_t ret;
  if (IS_ERR_VALUE(uaddr)) {
    filp_close(file, NULL);
    return (ssize_t)uaddr;
  }

  if (copy_to_user((void __user *)uaddr, data, len)) {
    ret = -EFAULT;
    goto out;
  }

  loff_t pos = 0;
  ret = file->f_op->write(file, (const char __user *)uaddr, len, &pos);

out:
  vm_munmap(uaddr, PAGE_SIZE);
  filp_close(file, NULL);
  return ret;
}

#define DEBUGFS_SCALING "/sys/kernel/debug/sched/tunable_scaling"

static void run(void) {
  // First, verify that writing a valid value works (sanity check)
  ssize_t ret_valid =
      debugfs_try_write(DEBUGFS_SCALING, "1\n", 2);
  TRACE_INFO("write '1' (valid) returned %zd", ret_valid);

  // Write an out-of-range value: SCHED_TUNABLESCALING_END == 3
  // Buggy kernel: no range check, kstrtouint directly writes to
  //   sysctl_sched_tunable_scaling → succeeds (returns 2)
  // Fixed kernel: range check rejects value >= 3 → fails (-EINVAL)
  ssize_t ret_oob =
      debugfs_try_write(DEBUGFS_SCALING, "3\n", 2);
  TRACE_INFO("write '3' (out-of-range) returned %zd", ret_oob);

  // Restore a valid value
  debugfs_try_write(DEBUGFS_SCALING, "1\n", 2);

  if (ret_oob > 0) {
    kstep_fail("out-of-range write accepted (no range validation)");
  } else {
    kstep_pass("out-of-range write rejected (range validation works)");
  }
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "null_term_scaling",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
