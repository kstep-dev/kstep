// https://github.com/torvalds/linux/commit/703066188f63
//
// Bug: sched_scaling_write() fails to null-terminate the buffer before calling
// kstrtouint(), causing all writes to /sys/kernel/debug/sched/tunable_scaling
// to fail with -EINVAL regardless of the input value.

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 15, 0)

static struct task_struct *helper;

static void setup(void) {
  helper = kstep_task_create();
  kstep_task_pin(helper, 1, 1);
  kstep_task_wakeup(helper);
  kstep_tick_repeat(5);
}

// Write to a debugfs file from kernel context by borrowing a user task's mm.
// Returns the result of file->f_op->write (positive on success, negative errno on failure).
static ssize_t debugfs_try_write(const char *path, const char *data,
                                 size_t len) {
  struct file *file = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(file))
    return PTR_ERR(file);

  if (!file->f_op || !file->f_op->write) {
    filp_close(file, NULL);
    return -ENOSYS;
  }

  struct mm_struct *mm = helper->mm;
  if (!mm) {
    filp_close(file, NULL);
    return -EINVAL;
  }

  kthread_use_mm(mm);

  unsigned long uaddr =
      vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
              MAP_ANONYMOUS | MAP_PRIVATE, 0);
  ssize_t ret;
  if (IS_ERR_VALUE(uaddr)) {
    ret = (ssize_t)uaddr;
    goto out_unuse;
  }

  if (copy_to_user((void __user *)uaddr, data, len)) {
    ret = -EFAULT;
    goto out_munmap;
  }

  loff_t pos = 0;
  ret = file->f_op->write(file, (const char __user *)uaddr, len, &pos);

out_munmap:
  vm_munmap(uaddr, PAGE_SIZE);
out_unuse:
  kthread_unuse_mm(mm);
  filp_close(file, NULL);
  return ret;
}

static void run(void) {
  unsigned int initial = sysctl_sched_tunable_scaling;
  TRACE_INFO("initial tunable_scaling = %u", initial);

  // Write "0\n" to tunable_scaling (same as 'echo 0')
  // On buggy kernel: returns -EINVAL (missing null terminator)
  // On fixed kernel: returns 2 (success)
  ssize_t ret = debugfs_try_write(
      "/sys/kernel/debug/sched/tunable_scaling", "0\n", 2);
  TRACE_INFO("write '0' returned %zd", ret);

  unsigned int after = sysctl_sched_tunable_scaling;
  TRACE_INFO("after tunable_scaling = %u", after);

  if (ret < 0) {
    kstep_fail("write failed with %zd (missing null termination)", ret);
  } else {
    kstep_pass("write succeeded (ret=%zd, scaling=%u->%u)", ret, initial,
               after);
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
