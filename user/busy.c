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
#include <sys/stat.h>
#include <sys/syscall.h> // SYS_clone3
#include <unistd.h>
#include <sys/resource.h> // for setpriority

#include "../kmod/sigcode.h"

#define CGROUP_ROOT "/sys/fs/cgroup"

void clone3(int val, const char *cgroup_path) {
  for (int i = 0; i < val; i++) {
    int cgfd = open(cgroup_path, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (cgfd < 0) {
      perror("open cgroup");
      return;
    }

    struct clone_args args;
    memset(&args, 0, sizeof(args));
    args.flags = 0 | CLONE_INTO_CGROUP;
    args.cgroup = (uintptr_t)cgfd; // <— CLONE_INTO_CGROUP: target cgroup fd

    pid_t pid = syscall(SYS_clone3, &args, sizeof(args));
    if (pid < 0) {
      perror("clone3");
      return;
    }
    if (pid == 0) {
      return;
    }
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
          cpu_set_t cpuset;
          CPU_ZERO(&cpuset);
          CPU_SET(val2, &cpuset);
          int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
          if (s != 0) {
            perror("sched_setaffinity");
          }
        } else if (code == SIGCODE_FORK_PIN_RANGE) {
          cpu_set_t cpuset;
          CPU_ZERO(&cpuset);
          for (int i = val2; i <= val3; i++) {
            CPU_SET(i, &cpuset);
          }
          int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
          if (s != 0) {
            perror("sched_setaffinity");
          }
        } else if (code == SIGCODE_FORK_FF) {
          struct sched_param sp = { .sched_priority = 80 };
          if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            perror("sched_setscheduler");
          }
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
    if (setpriority(PRIO_PROCESS, 0, val) == -1) {
      perror("setpriority");
    }
  } else if (code == SIGCODE_PIN) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(val, &cpuset);
    if (val2 == 0) {
      CPU_SET(val, &cpuset);
    } else {
      for (int i = val; i <= val2; i++)
        CPU_SET(i, &cpuset);
    }
    int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    if (s != 0) {
      perror("sched_setaffinity");
    }
  } else {
    printf("Unknown signal code: %d\n", code);
  }
}

static void set_proc_affinity() {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  int nproc = sysconf(_SC_NPROCESSORS_ONLN);
  for (int i = 1; i < nproc; i++) { // skip cpu 0
    CPU_SET(i, &cpuset);
  }
  int s = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  if (s != 0) {
    perror("sched_setaffinity");
  }
}

static void loop() {
  while (1)
    __asm__("" : : : "memory");
}

int main() {
  struct sigaction sa = {.sa_sigaction = signal_handler,
                         .sa_flags = SA_SIGINFO};
  sigaction(SIGUSR1, &sa, NULL);
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  } else if (pid == 0) {
    // Child process
    printf("test-proc started\n");
    set_proc_affinity();
    prctl(PR_SET_NAME, "test-proc");
    pause();
    loop();
    exit(0);
  }
  return 0;
}
