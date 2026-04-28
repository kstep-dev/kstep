#include <linux/cpumask.h>
#include <linux/cgroup.h>
#include <linux/dcache.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/percpu-rwsem.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "internal.h"

#define SYSCTL_ROOT "/proc/sys/"
#define CGROUP_ROOT "/sys/fs/cgroup/"
#define CGROUP_CONTROL "+cpu +cpuset"

#define MAX_PATH_LENGTH 512
#define MAX_DATA_LENGTH 512

void kstep_write(const char *path, const char *buf, size_t size) {
  TRACE_INFO("Writing %s: %s", path, buf);
  struct file *file = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(file))
    panic("open %s failed: %ld", path, PTR_ERR(file));

  loff_t pos = 0;
  ssize_t ret = kernel_write(file, buf, size, &pos);
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

static const char *const kstep_sched_feat_names[] = {
#define SCHED_FEAT(name, enabled) [__SCHED_FEAT_##name] = #name,
#include <kernel/sched/features.h>
#undef SCHED_FEAT
};

static unsigned int *kstep_sysctl_sched_features_ptr(void) {
  static unsigned int *ptr;

  if (!ptr)
    ptr = kstep_ksym_lookup("sysctl_sched_features");
  if (!ptr)
    panic("failed to resolve sysctl_sched_features");
  return ptr;
}

static int kstep_sched_feat_index(const char *name) {
  for (int i = 0; i < __SCHED_FEAT_NR; i++) {
    if (kstep_sched_feat_names[i] && !strcmp(kstep_sched_feat_names[i], name))
      return i;
  }
  return -1;
}

static void kstep_sched_feat_set(const char *name, bool enabled) {
  unsigned int *sysctl = kstep_sysctl_sched_features_ptr();
  unsigned int mask;
  int idx = kstep_sched_feat_index(name);

  if (idx < 0)
    return;

  mask = 1U << idx;
  if (!!(*sysctl & mask) == enabled)
    return;

  if (enabled) {
    *sysctl |= mask;
  } else {
    *sysctl &= ~mask;
  }

  TRACE_INFO("%s sched feature %s", enabled ? "Enabled" : "Disabled", name);
}

void kstep_sched_feat_write(const char *fmt, ...) {
  char data[MAX_DATA_LENGTH] = {0};
  char *name = data;
  bool enabled = true;

  va_list args;
  va_start(args, fmt);
  int size = vsnprintf(data, sizeof(data), fmt, args);
  va_end(args);
  if (size <= 0 || size >= sizeof(data))
    panic("failed to format sched feature write");

  if (!strncmp(name, "NO_", 3)) {
    enabled = false;
    name += 3;
  }

  kstep_sched_feat_set(name, enabled);
}

void kstep_sched_feat_enable(const char *name) {
  kstep_sched_feat_write("%s", name);
  kstep_sleep();
}

void kstep_sched_feat_disable(const char *name) {
  kstep_sched_feat_write("NO_%s", name);
  kstep_sleep();
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

static void kstep_rmdir(const char *dir) {
  struct path path;
  struct inode *parent;
  int err;

  err = kern_path(dir, LOOKUP_DIRECTORY, &path);
  if (err)
    panic("kern_path %s failed: %d", dir, err);

  err = mnt_want_write(path.mnt);
  if (err) {
    path_put(&path);
    panic("mnt_want_write %s failed: %d", dir, err);
  }

  parent = d_inode(path.dentry->d_parent);
  inode_lock_nested(parent, I_MUTEX_PARENT);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  err = vfs_rmdir(&nop_mnt_idmap, parent, path.dentry, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
  err = vfs_rmdir(&nop_mnt_idmap, parent, path.dentry);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
  err = vfs_rmdir(&init_user_ns, parent, path.dentry);
#else
  err = vfs_rmdir(parent, path.dentry);
#endif

  inode_unlock(parent);
  mnt_drop_write(path.mnt);
  path_put(&path);

  if (err)
    panic("rmdir %s failed: %d", dir, err);

  TRACE_INFO("Removed directory %s", dir);
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
  char cpuset[32];
  kstep_cgroup_mkdir(name);
  kstep_cgroup_write(name, "cgroup.subtree_control", CGROUP_CONTROL);
  if (scnprintf(cpuset, sizeof(cpuset), "%d-%d", 1, num_online_cpus() - 1) >= sizeof(cpuset))
    panic("failed to form cpuset for %s", name);

  kstep_cgroup_set_cpuset(name, cpuset);
}

void kstep_cgroup_destroy(const char *name) {
  char path[MAX_PATH_LENGTH] = {0};
  struct cgroup *cgrp;
  struct cgroup_subsys_state *css = NULL;
  struct task_group *tg = NULL;
  typedef void(unregister_fair_sched_group_type)(struct task_group *tg);
  KSYM_IMPORT_TYPED(unregister_fair_sched_group_type,
                    unregister_fair_sched_group);

  if (!name[0])
    panic("refusing to destroy root cgroup");

  int ret = scnprintf(path, sizeof(path), CGROUP_ROOT "%s", name);
  if (ret <= 0 || ret >= sizeof(path))
    panic("failed to form cgroup path for %s", name);

  cgrp = cgroup_get_from_path(name);
  if (IS_ERR(cgrp))
    panic("cgroup_get_from_path %s failed: %ld", path, PTR_ERR(cgrp));

  rcu_read_lock();
  css = rcu_dereference(cgrp->subsys[cpu_cgrp_id]);
  if (css)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    tg = css_tg(css);
#else
    tg = css ? container_of(css, struct task_group, css) : NULL;
#endif
  rcu_read_unlock();

  kstep_rmdir(path);
  kstep_sleep();

  if (tg)
    KSYM_unregister_fair_sched_group(tg);

  cgroup_put(cgrp);
  kstep_sleep();
}

void kstep_cgroup_set_cpuset(const char *name, const char *cpuset) {
  kstep_cgroup_write(name, "cpuset.cpus", "%s", cpuset);
  kstep_sleep();
}

void kstep_cgroup_set_weight(const char *name, int weight) {
  kstep_cgroup_write(name, "cpu.weight", "%d", weight);
  kstep_sleep();
}

void kstep_cgroup_add_task(const char *name, int pid) {
  kstep_cgroup_write(name, "cgroup.procs", "%d", pid);
  kstep_sleep();
}

void kstep_freeze_task(struct task_struct *p) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
  if (READ_ONCE(p->__state) & TASK_FROZEN) {
# else
  if (READ_ONCE(p->flags) & PF_FROZEN) {
# endif
    TRACE_INFO("Task %d already frozen", p->pid);
    return;
  }

// https://github.com/torvalds/linux/commit/f5d39b020809146cc28e6e73369bf8065e0310aa
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
  static_branch_dec(&freezer_active);
#else
  atomic_dec(&system_freezing_cnt);
#endif
}

void kstep_thaw_task(struct task_struct *p) {
  KSYM_IMPORT(__thaw_task);

  TRACE_INFO("Thawing task %d", p->pid);
  KSYM___thaw_task(p);
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
