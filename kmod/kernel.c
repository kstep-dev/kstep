#include <linux/cpumask.h>
#include <linux/dcache.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kstep.h"

#define CGROUP_ROOT "/sys/fs/cgroup/"
#define CGROUP_CONTROL "+cpu +cpuset"

#define MAX_CGROUP_PATH_LENGTH 64
#define MAX_CGROUP_DATA_LENGTH 64

void kstep_write(const char *path, const char *buf, size_t size) {
  TRACE_INFO("Writing %s: %s", path, buf);
  struct file *file = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(file))
    panic("open %s failed: %ld", path, PTR_ERR(file));

  loff_t pos = 0;
  if (kernel_write(file, buf, size, &pos) < 0)
    panic("write %s failed: %s", path, buf);

  filp_close(file, NULL);
}

void kstep_mkdir(const char *dir) {
  struct path path;
  int flags = LOOKUP_DIRECTORY;
// https://github.com/torvalds/linux/commit/3d18f80ce181ba27f37d0ec1c550b22acb01dd49
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  struct dentry *dentry = start_creating_path(AT_FDCWD, dir, &path, flags);
#else
  struct dentry *dentry = kern_path_create(AT_FDCWD, dir, &path, flags);
#endif
  if (IS_ERR(dentry))
    panic("kern_path_create %s failed: %ld", dir, PTR_ERR(dentry));

  struct inode *inode = d_inode(path.dentry);
// https://github.com/torvalds/linux/commit/c54b386969a58151765a9ffaaa0438e7b580283f
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
  struct dentry *result = vfs_mkdir(&nop_mnt_idmap, inode, dentry, 0755);
  int err = IS_ERR(result);
// https://github.com/torvalds/linux/commit/abf08576afe31506b812c8c1be9714f78613f300
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
  int err = vfs_mkdir(&nop_mnt_idmap, inode, dentry, 0755);
// https://github.com/torvalds/linux/commit/6521f8917082928a4cb637eb64b77b5f2f5b30fc
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
  int err = vfs_mkdir(&init_user_ns, inode, dentry, 0755);
#else
  int err = vfs_mkdir(inode, dentry, 0755);
#endif
  if (err)
    panic("mkdir %s failed: %d", dir, err);

  // https://github.com/torvalds/linux/commit/3d18f80ce181ba27f37d0ec1c550b22acb01dd49
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  end_creating_path(&path, dentry);
#else
  done_path_create(&path, dentry);
#endif

  TRACE_INFO("Created directory %s", dir);
}

void kstep_cgroup_write(const char *name, const char *filename, const char *fmt,
                        ...) {
  char data[MAX_CGROUP_DATA_LENGTH] = {0};
  char path[MAX_CGROUP_PATH_LENGTH] = {0};

  // Format data
  va_list args;
  va_start(args, fmt);
  int size = vsnprintf(data, sizeof(data), fmt, args);
  va_end(args);
  if (size <= 0 || size >= sizeof(data))
    panic("failed to format cgroup data for %s", filename);

  // Format path
  int ret = scnprintf(path, sizeof(path), CGROUP_ROOT "%s/%s", name, filename);
  if (ret <= 0 || ret >= sizeof(path))
    panic("failed to form cgroup file path for %s", filename);

  kstep_write(path, data, size);
}

static void kstep_cgroup_mkdir(const char *name) {
  char path[MAX_CGROUP_PATH_LENGTH] = {0};
  int ret = scnprintf(path, sizeof(path), CGROUP_ROOT "%s", name);
  if (ret <= 0 || ret >= sizeof(path))
    panic("failed to form cgroup file path for %s", name);
  kstep_mkdir(path);
}

void kstep_cgroup_create_pinned(const char *name, const char *cpuset) {
  static bool root_initialized = false;
  if (!root_initialized) {
    kstep_cgroup_write("", "cgroup.subtree_control", CGROUP_CONTROL);
    root_initialized = true;
  }

  kstep_cgroup_mkdir(name);
  kstep_cgroup_write(name, "cgroup.subtree_control", CGROUP_CONTROL);
  kstep_cgroup_write(name, "cpuset.cpus", "%s", cpuset);
}

void kstep_cgroup_set_weight(const char *name, int weight) {
  kstep_cgroup_write(name, "cpu.weight", "%d", weight);
}

void kstep_cgroup_add_task(const char *name, int pid) {
  kstep_cgroup_write(name, "cgroup.procs", "%d", pid);
}

void kstep_freeze_task(struct task_struct *p) {
// https://github.com/torvalds/linux/commit/f5d39b020809146cc28e6e73369bf8065e0310aa
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
  static_branch_inc(&freezer_active);
#else
  atomic_inc(&system_freezing_cnt);
#endif

  *ksym.pm_freezing = true;

  TRACE_INFO("Freezing task %d", p->pid);
  ksym.freeze_task(p);

  *ksym.pm_freezing = false;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
  static_branch_dec(&freezer_active);
#else
  atomic_dec(&system_freezing_cnt);
#endif
}

int kstep_eligible(struct sched_entity *se) {
  return ksym.entity_eligible(se->cfs_rq, se);
}
