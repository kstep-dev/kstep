#define _GNU_SOURCE

#include <errno.h>       // EEXIST
#include <fcntl.h>       // open, O_RDONLY, O_RDWR, O_NOCTTY
#include <limits.h>      // INT_MAX
#include <sched.h>       // sched_setaffinity, cpu_set_t
#include <signal.h>      // sigaction
#include <stdio.h>       // fprintf
#include <string.h>      // strcmp
#include <sys/mount.h>   // mount
#include <sys/reboot.h>  // reboot
#include <sys/stat.h>    // mkdir
#include <sys/syscall.h> // SYS_*
#include <termios.h>     // termios, tcgetattr, tcsetattr
#include <time.h>        // nanosleep, struct timespec
#include <unistd.h>      // close, getpid, syscall, fork, pause, _exit

#include "user.h" // SIGCODE_*, KSTEP_CTRL_FD

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
  if (fd < 0) { // a virtio port only exists when QEMU attaches it (e.g. the fuzz socket)
    fprintf(stderr, "Skipping %s: %s\n", path, strerror(errno));
    return;
  }

  struct termios termios;
  if (tcgetattr(fd, &termios) < 0)
    return;

  cfmakeraw(&termios);
  if (tcsetattr(fd, TCSANOW, &termios) < 0)
    panic("Failed to tcsetattr %s", path);
  // Deliberately left open: hvc resets the termios when the last file on the tty closes
  // (TTY_DRIVER_RESET_TERMIOS), and init lives as long as the guest anyway.
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
  set_tty_raw_output("/dev/hvc0"); // JSON out, and commands in for the cli driver
  set_tty_raw_output("/dev/hvc1"); // For code coverage data
  set_tty_raw_output("/dev/hvc2"); // For the fuzz executor's command socket (only attached when fuzzing)
  load_kmod("kmod.ko", argc, argv, envp);
  panic("Kernel module exited unexpectedly");
}

// ============================================================================
// PROGRAM 2 — /task  (worker, spawned by kmod)
// ============================================================================

// The pipe is the FIFO init created; a task opens it read-write on first use, so a read
// blocks instead of hitting EOF and a write never sees EPIPE.
static int pipe_fd(void) {
  static int fd;
  if (!fd && (fd = open(PIPE_PATH, O_RDWR)) < 0)
    panic("Failed to open %s", PIPE_PATH);
  return fd;
}

static void handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  if (code == SIGCODE_WAKEUP)
    return;
  else if (code == SIGCODE_WAIT) {
    char c;
    read(pipe_fd(), &c, 1); // wait: sleeps in pipe_read() until a token or a signal arrives
  } else if (code == SIGCODE_POST)
    write(pipe_fd(), "w", 1); // post: pipe_write() sync-wakes one waiter, from this CPU
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

__attribute__((noreturn)) static int task_main(void) {
  struct sigaction sa = {.sa_sigaction = handler,
                         .sa_flags = SA_SIGINFO | SA_NODEFER};
  sigaction(SIGUSR1, &sa, NULL);
  write(KSTEP_CTRL_FD, NULL, 0); // park until the first wakeup (see kstep_ctrl_write)
  while (1)
    read(KSTEP_CTRL_FD, NULL, 0); // run: halt until told otherwise (see kstep_ctrl_read)
}

// ============================================================================
// ENTRY POINT
// ============================================================================

int main(int argc, char **argv, char **envp) {
  if (argc > 0 && !strcmp(argv[0], "task"))
    return task_main();
  return init_main(argc, argv, envp);
}
