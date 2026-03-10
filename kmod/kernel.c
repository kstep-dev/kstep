#include <linux/cpumask.h>
#include <linux/dcache.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/kernfs.h>
#include <linux/namei.h>
#include <linux/percpu-rwsem.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "internal.h"

#define SYSCTL_ROOT "/proc/sys/"
#define CGROUP_ROOT "/sys/fs/cgroup/"
#define CGROUP_CONTROL "+cpu +cpuset"

#define MAX_PATH_LENGTH 64
#define MAX_DATA_LENGTH 64

// Direct write for kernfs-backed files where kernel_write doesn't work
// (kernfs uses .write which calls copy_from_user, failing with kernel bufs)
static ssize_t kstep_kernfs_write(struct file *file, const char *buf,
                                  size_t count) {
  struct seq_file *sf = file->private_data;
  struct kernfs_open_file *of = sf->private;
  const struct kernfs_ops *ops = of->kn->attr.ops;
  ssize_t len;

  if (!ops || !ops->write)
    return -EINVAL;

  if (of->atomic_write_len) {
    len = count;
    if (len > of->atomic_write_len)
      return -E2BIG;
  } else {
    len = min_t(size_t, count, PAGE_SIZE);
  }

  char *kbuf = kmalloc(len + 1, GFP_KERNEL);
  if (!kbuf)
    return -ENOMEM;
  memcpy(kbuf, buf, len);
  kbuf[len] = '\0';

  mutex_lock(&of->mutex);
  len = ops->write(of, kbuf, len, 0);
  mutex_unlock(&of->mutex);

  kfree(kbuf);
  return len;
}

void kstep_write(const char *path, const char *buf, size_t size) {
  TRACE_INFO("Writing %s: %s", path, buf);
  struct file *file = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(file))
    panic("open %s failed: %ld", path, PTR_ERR(file));

  loff_t pos = 0;
  ssize_t ret;
  // kernel_write needs write_iter without write; use direct kernfs path otherwise
  if (file->f_op->write_iter && !file->f_op->write)
    ret = kernel_write(file, buf, size, &pos);
  else
    ret = kstep_kernfs_write(file, buf, size);
  if (ret < 0)
    panic("write %s failed with return value %ld", path, ret);

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

// https://github.com/torvalds/linux/commit/e12d203b8c880061c0bf0339cad51e5851a33442
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  struct dentry *result = vfs_mkdir(&nop_mnt_idmap, inode, dentry, 0755, NULL);
  int err = IS_ERR(result) ? PTR_ERR(result) : 0;
// https://github.com/torvalds/linux/commit/c54b386969a58151765a9ffaaa0438e7b580283f
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
  struct dentry *result = vfs_mkdir(&nop_mnt_idmap, inode, dentry, 0755);
  int err = IS_ERR(result) ? PTR_ERR(result) : 0;
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

void kstep_sysctl_write(const char *name, const char *fmt, ...) {
  char data[MAX_DATA_LENGTH] = {0};
  char path[MAX_PATH_LENGTH] = {0};

  // Format data with "\n" at the end
  va_list args;
  va_start(args, fmt);
  int size = vsnprintf(data, sizeof(data) - 1, fmt, args);
  va_end(args);
  if (size <= 0 || size >= sizeof(data) - 1)
    panic("failed to format sysctl data for %s", name);
  data[size++] = '\n';
  data[size] = '\0';

  // Format path with "." replaced by "/"
  size_t sysctl_root_len = strlen(SYSCTL_ROOT);
  size_t name_len = strlen(name);
  if (sysctl_root_len + name_len >= sizeof(path))
    panic("failed to form sysctl file path for %s", name);
  memcpy(path, SYSCTL_ROOT, sysctl_root_len);
  for (size_t i = 0; i < name_len; i++)
    path[sysctl_root_len + i] = (name[i] == '.') ? '/' : name[i];

  kstep_write(path, data, size);
}

void kstep_cgroup_write(const char *name, const char *filename, const char *fmt,
                        ...) {
  char data[MAX_DATA_LENGTH] = {0};
  char path[MAX_PATH_LENGTH] = {0};

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
  char path[MAX_PATH_LENGTH] = {0};
  int ret = scnprintf(path, sizeof(path), CGROUP_ROOT "%s", name);
  if (ret <= 0 || ret >= sizeof(path))
    panic("failed to form cgroup file path for %s", name);
  kstep_mkdir(path);
}

void kstep_cgroup_init(void) {
  kstep_cgroup_write("", "cgroup.subtree_control", CGROUP_CONTROL);

  // Pre-enter rcu_sync on cgroup percpu rwsems so that subsequent
  // percpu_down_write calls skip synchronize_rcu(), which would hang
  // because ticks are disabled on CPU1-N.
  KSYM_IMPORT(rcu_sync_enter);
  KSYM_IMPORT(cgroup_threadgroup_rwsem);
  KSYM_rcu_sync_enter(&KSYM_cgroup_threadgroup_rwsem->rss);

  struct percpu_rw_semaphore *cpuset_rwsem = kstep_ksym_lookup("cpuset_rwsem");
  if (cpuset_rwsem)
    KSYM_rcu_sync_enter(&cpuset_rwsem->rss);
}

void kstep_cgroup_create(const char *name) {
  kstep_cgroup_mkdir(name);
  kstep_cgroup_write(name, "cgroup.subtree_control", CGROUP_CONTROL);
}

void kstep_cgroup_set_cpuset(const char *name, const char *cpuset) {
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
  static_branch_inc(&freezer_active);
#else
  atomic_inc(&system_freezing_cnt);
#endif

  KSYM_IMPORT(pm_freezing);
  KSYM_IMPORT(freeze_task);

  *KSYM_pm_freezing = true;

  TRACE_INFO("Freezing task %d", p->pid);
  KSYM_freeze_task(p);

  *KSYM_pm_freezing = false;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
  static_branch_dec(&freezer_active);
#else
  atomic_dec(&system_freezing_cnt);
#endif
}

int kstep_eligible(struct sched_entity *se) {
// https://github.com/torvalds/linux/commit/147f3efaa24182a21706bca15eab2f3f4630b5fe
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  KSYM_IMPORT(entity_eligible);
  return KSYM_entity_eligible(se->cfs_rq, se);
#else
  panic("unsupported kernel");
#endif
}
