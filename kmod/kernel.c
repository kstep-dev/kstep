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
#define MAX_CGROUP_DATA_LENGTH 64

void kstep_write_file(const char *path, const char *buf, size_t size) {
  struct file *file = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(file))
    panic("open %s failed: %ld", path, PTR_ERR(file));

  loff_t pos = 0;
  if (kernel_write(file, buf, size, &pos) < 0)
    panic("write %s failed: %s", path, buf);

  filp_close(file, NULL);
  TRACE_INFO("Wrote %s: %s", path, buf);
}

void kstep_mkdir(int dfd, const char *dir) {
  struct path path;
  int flags = LOOKUP_DIRECTORY;
// https://github.com/torvalds/linux/commit/3d18f80ce181ba27f37d0ec1c550b22acb01dd49
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  struct dentry *dentry = start_creating_path(dfd, dir, &path, flags);
#else
  struct dentry *dentry = kern_path_create(dfd, dir, &path, flags);
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

int kstep_open_fd(const char *path, int flags) {
  struct file *file = filp_open(path, flags, 0);
  if (IS_ERR(file))
    panic("open %s failed: %ld", path, PTR_ERR(file));

  int fd = get_unused_fd_flags(0);
  if (fd < 0)
    panic("get_unused_fd_flags failed: %d", fd);
  fd_install(fd, file);

  return fd;
}

void kstep_close_fd(int fd) {
  struct file *file = fget_raw(fd);
  filp_close(file, NULL);
  put_unused_fd(fd);
}

static int cgroup_root_fd;

void kstep_cgroup_init(void) {
  cgroup_root_fd = kstep_open_fd(CGROUP_ROOT, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
  kstep_cgroup_write_raw("", "cgroup.subtree_control", CGROUP_CONTROL,
                         strlen(CGROUP_CONTROL));
}

void kstep_cgroup_write_raw(const char *dir, const char *filename,
                            const char *buf, size_t size) {
  char path[MAX_CGROUP_PATH_LENGTH];
  int ret = scnprintf(path, sizeof(path), CGROUP_ROOT "%s/%s", dir, filename);
  if (ret <= 0 || ret >= sizeof(path))
    panic("failed to form cgroup file path for %s", filename);
  kstep_write_file(path, buf, size);
}

void kstep_cgroup_write(const char *dir, const char *filename, const char *fmt,
                        ...) {
  va_list args;
  va_start(args, fmt);
  char buf[MAX_CGROUP_DATA_LENGTH];
  int size = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (size <= 0 || size >= sizeof(buf))
    panic("failed to format cgroup data for %s", filename);
  kstep_cgroup_write_raw(dir, filename, buf, size);
}

void kstep_cgroup_create(const char *dir) {
  kstep_mkdir(cgroup_root_fd, dir);
  kstep_cgroup_write_raw(dir, "cgroup.subtree_control", CGROUP_CONTROL,
                         strlen(CGROUP_CONTROL));
}
