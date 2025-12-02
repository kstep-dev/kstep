#include <linux/cpumask.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kstep.h"

#define CGROUP_ROOT "/sys/fs/cgroup/"
#define CGROUP_CONTROL "+cpu +cpuset"

#define MAX_CGROUP_PATH_LENGTH 64
#define MAX_CGROUP_FILE_LENGTH 64

static char default_cpuset[16];
static int cgroup_root_fd;

static int open_cgroup_root(void) {
  struct file *file =
      filp_open(CGROUP_ROOT, O_DIRECTORY | O_RDONLY | O_CLOEXEC, 0);
  if (IS_ERR(file))
    panic("open %s failed: %ld", CGROUP_ROOT, PTR_ERR(file));

  int fd = get_unused_fd_flags(0);
  if (fd < 0)
    panic("get_unused_fd_flags failed: %d", fd);
  fd_install(fd, file);

  return fd;
}

void kstep_cgroup_init(void) {
  snprintf(default_cpuset, sizeof(default_cpuset), "1-%d",
           num_online_cpus() - 1);
  cgroup_root_fd = open_cgroup_root();
  kstep_cgroup_write_file("", "cgroup.subtree_control", CGROUP_CONTROL);
}

static struct file *kstep_cgroup_open_file(const char *dir,
                                           const char *filename) {
  char path[MAX_CGROUP_PATH_LENGTH];
  int ret = scnprintf(path, sizeof(path), CGROUP_ROOT "%s/%s", dir, filename);
  if (ret <= 0 || ret >= sizeof(path))
    panic("failed to form cgroup file path for %s", filename);

  struct file *file = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(file))
    panic("open %s failed: %ld", path, PTR_ERR(file));
  return file;
}

void kstep_cgroup_write_file(const char *dir, const char *filename,
                             const char *fmt, ...) {
  struct file *file = kstep_cgroup_open_file(dir, filename);

  char buf[MAX_CGROUP_FILE_LENGTH];
  va_list args;
  va_start(args, fmt);
  int len = vscnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len <= 0 || len >= sizeof(buf))
    panic("failed to format cgroup file path for %s", filename);

  loff_t pos = 0;
  if (kernel_write(file, buf, len, &pos) < 0)
    panic("write %s/%s: %s failed", dir, filename, buf);

  filp_close(file, NULL);

  TRACE_INFO("wrote %s/%s: %s", dir, filename, buf);
}

static void kstep_cgroup_mkdir(const char *dir) {
  struct path path;
// https://github.com/torvalds/linux/commit/3d18f80ce181ba27f37d0ec1c550b22acb01dd49
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  struct dentry *dentry =
      start_creating_path(cgroup_root_fd, dir, &path, LOOKUP_DIRECTORY);
#else
  struct dentry *dentry =
      kern_path_create(cgroup_root_fd, dir, &path, LOOKUP_DIRECTORY);
#endif
  if (IS_ERR(dentry))
    panic("kern_path_create %s failed: %ld", dir, PTR_ERR(dentry));

// https://github.com/torvalds/linux/commit/c54b386969a58151765a9ffaaa0438e7b580283f
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
  struct dentry *result =
      vfs_mkdir(&nop_mnt_idmap, d_inode(path.dentry), dentry, 0755);
  int err = PTR_ERR(result);
// https://github.com/torvalds/linux/commit/abf08576afe31506b812c8c1be9714f78613f300
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
  int err = vfs_mkdir(&nop_mnt_idmap, d_inode(path.dentry), dentry, 0755);
// https://github.com/torvalds/linux/commit/6521f8917082928a4cb637eb64b77b5f2f5b30fc
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
  int err = vfs_mkdir(&init_user_ns, d_inode(path.dentry), dentry, 0755);
#else
  int err = vfs_mkdir(d_inode(path.dentry), dentry, 0755);
#endif
  if (err)
    panic("mkdir %s failed: %d", dir, err);

  // https://github.com/torvalds/linux/commit/3d18f80ce181ba27f37d0ec1c550b22acb01dd49
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  end_creating_path(&path, dentry);
#else
  done_path_create(&path, dentry);
#endif

  TRACE_INFO("created cgroup %s", dir);
}

void kstep_cgroup_create(const char *path, const char *cpuset) {
  kstep_cgroup_mkdir(path);
  kstep_cgroup_write_file(path, "cgroup.subtree_control", CGROUP_CONTROL);
  kstep_cgroup_write_file(path, "cpuset.cpus",
                          cpuset ? cpuset : default_cpuset);
}
