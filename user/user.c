#define _GNU_SOURCE

#include <errno.h>       // EEXIST
#include <fcntl.h>       // open, O_RDONLY, O_RDWR
#include <limits.h>      // INT_MAX
#include <sched.h>       // sched_setaffinity, cpu_set_t
#include <signal.h>      // sigaction
#include <stdio.h>       // fprintf
#include <string.h>      // strcmp
#include <sys/mount.h>   // mount
#include <sys/reboot.h>  // reboot
#include <sys/stat.h>    // mkdir
#include <time.h>        // nanosleep, struct timespec
#include <sys/syscall.h> // SYS_*
#include <unistd.h>      // close, getpid, syscall, read, pause, _exit

#include "user.h" // KSTEP_CTRL_*, PIPE_PATH, KSTEP_CTRL_FD

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
  if (mkfifo(PIPE_PATH, 0600) < 0) // the tasks' wait/post semaphore (see user.h)
    panic("Failed to create %s", PIPE_PATH);
  mount_fs("/sys/fs/cgroup", "cgroup2");
  set_proc_affinity(0, 0);          // Bind to cpu 0
  // The kmod drives the JSON channel's virtio port itself (kmod/io.c); no tty to set up
  load_kmod("kmod.ko", argc, argv, envp);
  panic("Kernel module exited unexpectedly");
}

// ============================================================================
// PROGRAM 2 — /task  (worker, spawned by kmod)
// ============================================================================

// The channel is the FIFO init created; a task opens it read-write on first use, so a read blocks
// instead of hitting EOF and a write never sees EPIPE. Opened once and kept for the life of the
// task, which _exit() closes. The sentinel is -1, not 0: 0 is a real descriptor, and on a zero
// sentinel this would reopen on every call and leak one each time if it ever landed there.
static int chan_fd(void) {
  static int fd = -1;
  if (fd < 0 && (fd = open(PIPE_PATH, O_RDWR)) < 0)
    panic("Failed to open %s", PIPE_PATH);
  return fd;
}

// A wakeup is a bare SIGUSR1: it says nothing, and only returns the task from whatever it is
// asleep in. The handler exists so the signal does not kill the task instead.
static void handler(int signum) {}

__attribute__((noreturn)) static int task_main(void) {
  struct sigaction sa = {.sa_handler = handler};
  char c;

  sigaction(SIGUSR1, &sa, NULL);
  // A read returns the next thing the controller wants done, or halts. The halt is the task's
  // work, so a read that returns nothing has done it. The first read parks: a new task sleeps in
  // it until its first wakeup.
  while (1) {
    if (read(KSTEP_CTRL_FD, &c, sizeof(c)) <= 0)
      continue;
    switch (c) {
    case KSTEP_CTRL_EXIT:
      _exit(0);
    case KSTEP_CTRL_PAUSE:
      pause(); // interruptible, not freezable: returns when a wakeup interrupts it
      break;
    case KSTEP_CTRL_BLOCK:
      nanosleep(&(struct timespec){.tv_sec = INT_MAX}, NULL);
      break;
    case KSTEP_CTRL_CHAN_READ: {
      char token;
      read(chan_fd(), &token, 1); // sleeps in pipe_read() until there is a byte, and takes it
      break;
    }
    case KSTEP_CTRL_CHAN_WRITE:
      write(chan_fd(), "w", 1); // pipe_write() sync-wakes one reader, from this CPU
      break;
    case KSTEP_CTRL_YIELD:
      sched_yield(); // stays runnable: the class decides who runs instead, if anyone
      break;
    default:
      panic("Unknown control action: %d", c);
    }
  }
}

// ============================================================================
// ENTRY POINT
// ============================================================================

int main(int argc, char **argv, char **envp) {
  if (argc > 0 && !strcmp(argv[0], "task"))
    return task_main();
  return init_main(argc, argv, envp);
}
