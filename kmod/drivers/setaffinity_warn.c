// https://github.com/torvalds/linux/commit/70ee7947a290
//
// Bug: WARN_ON_ONCE fires in __sched_setaffinity when a cpuset update races
// with a per-task affinity assignment. When the cpuset changes between the two
// cpuset_cpus_allowed() calls in __sched_setaffinity, the user_mask has no
// overlap with the new cpuset mask, causing an empty intersection and a
// spurious warning.
//
// Fix: Replace WARN_ON_ONCE(empty) with if (empty) since this race is a
// normal, expected scenario with a proper fallback.

#include "driver.h"
#include "internal.h"
#include <linux/kthread.h>
#include <linux/kernfs.h>
#include <linux/seq_file.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 13, 0)

static struct task_struct *target;
static struct file *cpuset_file;
static atomic_t race_running;

// Write to the already-opened cpuset.cpus file without TRACE_INFO overhead
static void fast_cpuset_write(const char *cpus, size_t len) {
  struct seq_file *sf = cpuset_file->private_data;
  struct kernfs_open_file *of = sf->private;
  const struct kernfs_ops *ops = of->kn->attr.ops;
  char kbuf[8];

  if (!ops || !ops->write || len >= sizeof(kbuf))
    return;
  memcpy(kbuf, cpus, len);
  kbuf[len] = '\0';
  mutex_lock(&of->mutex);
  ops->write(of, kbuf, len, 0);
  mutex_unlock(&of->mutex);
}

static int toggler_fn(void *data) {
  while (atomic_read(&race_running)) {
    fast_cpuset_write("1-2", 3);
    fast_cpuset_write("1", 1);
  }
  return 0;
}

static int setter_fn(void *data) {
  pid_t pid = *(pid_t *)data;
  typedef long (*sa_fn_t)(pid_t, const struct cpumask *);
  sa_fn_t sa = (sa_fn_t)kstep_ksym_lookup("sched_setaffinity");

  if (!sa) {
    pr_err("kstep: could not find sched_setaffinity\n");
    return -1;
  }
  while (atomic_read(&race_running))
    sa(pid, cpumask_of(2));
  return 0;
}

static void setup(void) {
  kstep_cgroup_create("aff_cpuset");
  kstep_cgroup_set_cpuset("aff_cpuset", "1-2");

  target = kstep_task_create();
  kstep_cgroup_add_task("aff_cpuset", target->pid);
}

static void run(void) {
  struct task_struct *toggler, *setter;
  pid_t target_pid = target->pid;

  kstep_tick_repeat(5);

  // Open the cpuset.cpus file once for fast repeated writes
  cpuset_file = filp_open("/sys/fs/cgroup/aff_cpuset/cpuset.cpus", O_WRONLY, 0);
  if (IS_ERR(cpuset_file)) {
    kstep_fail("Could not open cpuset.cpus file");
    return;
  }

  TRACE_INFO("Starting race with target pid=%d", target_pid);
  atomic_set(&race_running, 1);

  toggler = kthread_create(toggler_fn, NULL, "toggler");
  if (IS_ERR(toggler)) {
    filp_close(cpuset_file, NULL);
    kstep_fail("Could not create toggler kthread");
    return;
  }
  set_cpus_allowed_ptr(toggler, cpumask_of(2));
  wake_up_process(toggler);

  setter = kthread_create(setter_fn, &target_pid, "setter");
  if (IS_ERR(setter)) {
    atomic_set(&race_running, 0);
    filp_close(cpuset_file, NULL);
    kstep_fail("Could not create setter kthread");
    return;
  }
  set_cpus_allowed_ptr(setter, cpumask_of(3));
  wake_up_process(setter);

  kstep_tick_repeat(200);

  atomic_set(&race_running, 0);
  kstep_tick_repeat(20);

  filp_close(cpuset_file, NULL);

  if (test_taint(TAINT_WARN))
    kstep_fail("WARN_ON_ONCE fired in sched_setaffinity: empty mask race");
  else
    kstep_pass("No warning fired");
}

KSTEP_DRIVER_DEFINE{
    .name = "setaffinity_warn",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "setaffinity_warn",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
#endif
