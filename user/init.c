#define _GNU_SOURCE

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_LINE 1024

#define panic(msg, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, msg "\n", ##__VA_ARGS__);                                  \
    exit(1);                                                                   \
  } while (0)

// Mount filesystems
void mount_fs(const char *dir, const char *type) {
  if (mkdir(dir, 0755) < 0 && errno != EEXIST)
    panic("Failed to create directory %s", dir);
  if (mount("none", dir, type, 0, "") < 0)
    panic("Failed to mount %s as %s", dir, type);
}

void set_cpu_affinity() {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) < 0)
    panic("Failed to set CPU affinity");
}

void insmod(const char *path, const char *params) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    panic("Failed to open %s", path);

  struct stat st;
  if (fstat(fd, &st) < 0)
    panic("Failed to fstat %s", path);

  off_t size = st.st_size;
  void *addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED)
    panic("Failed to mmap %s", path);

  close(fd);

  if (syscall(SYS_init_module, addr, size, params) < 0)
    panic("Failed to init_module %s", path);

  munmap(addr, size);
}

void run_kstep(int argc, char *argv[], char *envp[]) {
  char params[MAX_LINE] = {};
  for (int i = 1; i < argc; i++) { // Skip "/init"
    strlcat(params, argv[i], sizeof(params));
    strlcat(params, " ", sizeof(params));
  }
  for (int i = 2; envp[i] != NULL; i++) { // Skip HOME and TERM
    strlcat(params, envp[i], sizeof(params));
    strlcat(params, " ", sizeof(params));
  }
  printf("Running kSTEP with params: %s\n", params);
  insmod("kstep.ko", params);
}

int main(int argc, char *argv[], char *envp[]) {
  mount_fs("/proc", "proc");
  mount_fs("/sys", "sysfs");
  mount_fs("/sys/kernel/debug", "debugfs");
  mount_fs("/sys/fs/cgroup", "cgroup2");
  set_cpu_affinity();
  run_kstep(argc, argv, envp);
}
