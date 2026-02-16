#define _GNU_SOURCE

#include <string.h>      // strlcat
#include <sys/errno.h>   // EEXIST
#include <sys/mount.h>   // mount
#include <sys/stat.h>    // mkdir
#include <sys/syscall.h> // SYS_*
#include <termios.h>     // termios, tcgetattr, tcsetattr
#include <unistd.h>      // open, close, syscall

#include "utils.h"

#define MAX_PARAMS_LENGTH 512

static void mount_fs(const char *dir, const char *type) {
  if (mkdir(dir, 0755) < 0 && errno != EEXIST)
    panic("Failed to create directory %s", dir);
  if (mount("none", dir, type, 0, "") < 0)
    panic("Failed to mount %s as %s", dir, type);
}

static void load_kmod(const char *path, int argc, char *argv[], char *envp[]) {
  char params[MAX_PARAMS_LENGTH] = {};
  for (int i = 1; i < argc; i++) { // Skip "/init"
    strlcat(params, argv[i], sizeof(params));
    strlcat(params, " ", sizeof(params));
  }
  for (int i = 2; envp[i] != NULL; i++) { // Skip HOME and TERM
    strlcat(params, envp[i], sizeof(params));
    strlcat(params, " ", sizeof(params));
  }
  printf("Loading %s with params: %s\n", path, params);

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    panic("Failed to open %s", path);

  if (syscall(SYS_finit_module, fd, params, 0) < 0)
    panic("Failed to finit_module %s", path);

  close(fd);
}

static void set_tty_raw_output(const char *path) {
  int fd = open(path, O_RDWR | O_NOCTTY);
  if (fd < 0)
    panic("Failed to open %s", path);

  struct termios termios;
  if (tcgetattr(fd, &termios) < 0)
    panic("Failed to tcgetattr %s", path);

  termios.c_oflag &= ~(OPOST); // Disable output post-processing
  if (tcsetattr(fd, TCSANOW, &termios) < 0)
    panic("Failed to tcsetattr %s", path);

  close(fd);
}

int main(int argc, char *argv[], char *envp[]) {
  mount_fs("/dev", "devtmpfs");
  mount_fs("/proc", "proc");
  mount_fs("/sys", "sysfs");
  mount_fs("/sys/kernel/debug", "debugfs");
  mount_fs("/sys/fs/cgroup", "cgroup2");
  set_proc_affinity(0, 0); // bind to cpu 0
  set_tty_raw_output("/dev/ttyS2");
  load_kmod("kmod.ko", argc, argv, envp);
  panic("Kernel module exited unexpectedly");
}
