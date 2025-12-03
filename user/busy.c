#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/sched.h> // struct clone_args
#include <sched.h>
#include <signal.h>
#include <stdint.h> // for uintptr_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h> // for setpriority
#include <sys/stat.h>
#include <sys/syscall.h> // SYS_clone3
#include <unistd.h>

#include "../kmod/sigcode.h"

#define CGROUP_ROOT "/sys/fs/cgroup"

#define panic(msg, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, msg "\n", ##__VA_ARGS__);                                  \
    exit(1);                                                                   \
  } while (0)

static void set_proc_affinity(int begin, int end) { // [begin, end]
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (int i = begin; i <= end; i++)
    CPU_SET(i, &cpuset);
  if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
    panic("Failed to set CPU affinity to %d-%d", begin, end);
}

static void clone3(int val, const char *cgroup_path) {
  for (int i = 0; i < val; i++) {
    int cgfd = open(cgroup_path, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (cgfd < 0)
      panic("Failed to open cgroup %s", cgroup_path);

    struct clone_args args;
    memset(&args, 0, sizeof(args));
    args.flags = 0 | CLONE_INTO_CGROUP;
    args.cgroup = (uintptr_t)cgfd; // <— CLONE_INTO_CGROUP: target cgroup fd

    pid_t pid = syscall(SYS_clone3, &args, sizeof(args));
    if (pid < 0)
      panic("Failed to clone3");
    if (pid == 0) // child process
      return;
    close(cgfd);
  }
}
static void signal_handler(int signum, siginfo_t *info, void *context) {
  int code = info->si_code;
  int val = info->si_int;
  int val2 = info->si_pid;
  int val3 = info->si_uid;
  if (code == SIGCODE_FORK ||
      code == SIGCODE_FORK_PIN ||
      code == SIGCODE_FORK_FF ||
      code == SIGCODE_FORK_PIN_RANGE) {
    for (int i = 0; i < val; i++) {
      int pid = fork();
      if (pid == 0) {
        if (code == SIGCODE_FORK_PIN) {
          set_proc_affinity(val2, val2);
        } else if (code == SIGCODE_FORK_PIN_RANGE) {
          set_proc_affinity(val2, val3);
        } else if (code == SIGCODE_FORK_FF) {
          struct sched_param sp = { .sched_priority = 80 };
          if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            panic("sched_setscheduler failed");
          printf("set scheduler to FIFO with priority %d\n", sp.sched_priority);
        }
        return;
      }
    }
  } else if (code == SIGCODE_SLEEP) {
    sleep(val);
  } else if (code == SIGCODE_EXIT) {
    exit(0);
  } else if (code == SIGCODE_PAUSE) {
    pause();
  } // TODO: generalize the logic of clone3 to different cgroup
  else if (code == SIGCODE_CLONE3_L3_0) {
    clone3(val, CGROUP_ROOT "/l1_0/l2_0/l3_0");
  } else if (code == SIGCODE_CLONE3_L3_1) {
    clone3(val, CGROUP_ROOT "/l1_0/l2_0/l3_1");
  } else if (code == SIGCODE_CLONE3_L2_1) {
    clone3(val, CGROUP_ROOT "/l1_0/l2_1");
  } else if (code == SIGCODE_CLONE3_L1_0) {
    clone3(val, CGROUP_ROOT "/l1_0");
  } else if (code == SIGCODE_REWEIGHT) {
    if (setpriority(PRIO_PROCESS, 0, val) < 0)
      panic("setpriority failed");
  } else if (code == SIGCODE_PIN) {
    set_proc_affinity(val, val2 == 0 ? val : val2);
  } else {
    printf("Unknown signal code: %d\n", code);
  }
}

static void loop() {
  while (1)
    __asm__("" : : : "memory");
}

int main() {
  printf("busy task started\n");
  struct sigaction sa = {.sa_sigaction = signal_handler,
                         .sa_flags = SA_SIGINFO};
  sigaction(SIGUSR1, &sa, NULL);
  set_proc_affinity(1, sysconf(_SC_NPROCESSORS_ONLN) - 1);
  prctl(PR_SET_NAME, "test-proc");
  pause();
  loop();
}
