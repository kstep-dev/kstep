#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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

void mount_filesystems() {
  mount_fs("/proc", "proc");
  mount_fs("/sys", "sysfs");
  mount_fs("/sys/kernel/debug", "debugfs");
  mount_fs("/sys/fs/cgroup", "cgroup2");
}

void set_cpu_affinity() {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) < 0)
    panic("Failed to set CPU affinity");
}

void run_prog(char *args[]) {
  printf("Running %s\n", args[0]);
  pid_t pid = fork();

  if (pid == 0) { // Child process
    execvp(args[0], args);
    panic("Failed to exec %s", args[0]);
  } else if (pid > 0) { // Parent process
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
      int exit_status = WEXITSTATUS(status);
      if (exit_status != 0) {
        panic("Command %s exited with status %d", args[0], exit_status);
      }
    } else {
      panic("Command %s exited with unknown status %d", args[0], status);
    }
  } else {
    panic("Failed to fork");
  }
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
  for (int i = 2; i < argc; i++) { // Skip "/init" and "-"
    strlcat(params, argv[i], sizeof(params));
    strlcat(params, " ", sizeof(params));
  }
  for (int i = 2; envp[i] != NULL; i++) { // Skip HOME and PATH
    strlcat(params, envp[i], sizeof(params));
    strlcat(params, " ", sizeof(params));
  }
  printf("Running kstep with params: %s\n", params);
  insmod("kmod.ko", params);
}

int main(int argc, char *argv[], char *envp[]) {
  printf("\n");
  printf("Welcome to kSTEP\n");

  mount_filesystems();
  set_cpu_affinity();

  run_kstep(argc, argv, envp);
  run_prog((char *[]){"/cgroup", NULL});
  run_prog((char *[]){"/busy", NULL});
  // waitpid in init should be called before set SIG_IGN
  signal(SIGCHLD, SIG_IGN);
  pause();
  exit(0);
}
