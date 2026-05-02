#define _GNU_SOURCE

#include <errno.h>       // EEXIST
#include <fcntl.h>       // open, O_RDONLY, O_RDWR, O_NOCTTY
#include <limits.h>      // INT_MAX
#include <sched.h>       // sched_setaffinity, cpu_set_t
#include <signal.h>      // sigaction
#include <stdio.h>       // fprintf
#include <string.h>      // strcmp
#include <sys/mount.h>   // mount
#include <sys/prctl.h>   // PR_SET_NAME
#include <sys/reboot.h>  // reboot
#include <sys/stat.h>    // mkdir
#include <sys/syscall.h> // SYS_*
#include <termios.h>     // termios, tcgetattr, tcsetattr
#include <time.h>        // nanosleep, struct timespec
#include <unistd.h>      // close, getpid, syscall, fork, pause, _exit

#include "user.h" // SIGCODE_*, TASK_READY_COMM

#define panic(msg, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, msg "\n", ##__VA_ARGS__);                                  \
    reboot(RB_AUTOBOOT);                                                       \
    __builtin_unreachable();                                                   \
  } while (0)

// ============================================================================
// PROGRAM 1 — /init  (PID 1, boot)
// ============================================================================

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

// Disable output post-processing
static void set_tty_raw_output(const char *path) {
  int fd = open(path, O_RDWR | O_NOCTTY);
  if (fd < 0)
    panic("Failed to open %s", path);

  struct termios termios;
  if (tcgetattr(fd, &termios) < 0)
    return;

  cfmakeraw(&termios);
  if (tcsetattr(fd, TCSANOW, &termios) < 0)
    panic("Failed to tcsetattr %s", path);

  close(fd);
}

static void set_proc_affinity(int begin, int end) { // [begin, end]
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (int i = begin; i <= end; i++)
    CPU_SET(i, &cpuset);
  if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
    panic("Failed to set CPU affinity for task %d to CPUs %d-%d", getpid(),
          begin, end);
}

static int init_main(int argc, char *argv[], char *envp[]) {
  mount_fs("/dev", "devtmpfs");
  mount_fs("/proc", "proc");
  mount_fs("/sys", "sysfs");
  mount_fs("/sys/kernel/debug", "debugfs");
  mount_fs("/sys/fs/cgroup", "cgroup2");
  set_proc_affinity(0, 0);          // Bind to cpu 0
  set_tty_raw_output("/dev/ttyS2"); // For code coverage data
  set_tty_raw_output("/dev/ttyS3"); // For interactive communication
  load_kmod("kmod.ko", argc, argv, envp);
  panic("Kernel module exited unexpectedly");
}

// ============================================================================
// PROGRAM 2 — /task  (worker, spawned by kmod)
// ============================================================================

static void handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  if (code == SIGCODE_WAKEUP)
    return;
  else if (code == SIGCODE_FORK) {
    for (int i = 0; i < val; i++) {
      int pid = fork();
      if (pid < 0)
        panic("fork failed at i == %d", i);
      if (pid == 0) // child process: stop forking further
        return;
    }
  } else if (code == SIGCODE_EXIT)
    _exit(0);
  else if (code == SIGCODE_PAUSE)
    pause();
  else if (code == SIGCODE_BLOCK)
    nanosleep(&(struct timespec){.tv_sec = INT_MAX}, NULL);
  else
    panic("Unknown signal code: %d", code);
}

__attribute__((noreturn)) static void loop() {
  while (1)
    __asm__("" : : : "memory");
}

static int task_main(int argc, char **argv) {
  struct sigaction sa = {.sa_sigaction = handler,
                         .sa_flags = SA_SIGINFO | SA_NODEFER};
  sigaction(SIGUSR1, &sa, NULL);
  prctl(PR_SET_NAME, TASK_READY_COMM);

  pause();
  loop();
}

// ============================================================================
// ENTRY POINT
// ============================================================================

int main(int argc, char **argv, char **envp) {
  if (argc > 0 && !strcmp(argv[0], "task"))
    return task_main(argc, argv);
  return init_main(argc, argv, envp);
}